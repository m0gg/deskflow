/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "EiKeyStateTests.h"

#include "base/EventQueue.h"
#include "deskflow/AppUtil.h"
#include "deskflow/KeyTypes.h"
#include "platform/EiKeyState.h"

#include <QByteArray>
#include <QTemporaryFile>

#include <xkbcommon/xkbcommon.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {
class TestAppUtil : public AppUtil
{
public:
  int run() override
  {
    return 0;
  }

  std::vector<std::string> getKeyboardLayoutList() override
  {
    return {"en"};
  }

  std::string getCurrentLanguageCode() override
  {
    return "en";
  }
};

const char TestKeymap[] = R"XKB(xkb_keymap {
xkb_keycodes "test" {
    minimum = 8;
    maximum = 255;
    <LFSH> = 50;
    <NMLK> = 77;
};
xkb_types "test" {
    type "ONE_LEVEL" {
        modifiers = none;
        level_name[Level1] = "Any";
    };
};
xkb_compat "test" {
    interpret Shift_L+AnyOfOrNone(all) {
        action = SetMods(modifiers=Shift);
    };
    interpret Num_Lock+AnyOfOrNone(all) {
        action = LockMods(modifiers=Mod2);
    };
};
xkb_symbols "test" {
    key <LFSH> { [ Shift_L ] };
    key <NMLK> { [ Num_Lock ] };
    modifier_map Shift { <LFSH> };
    modifier_map Mod2 { <NMLK> };
};
};)XKB";

// XKB keycodes for TestKeymap.
constexpr std::uint32_t LeftShiftKeycode = 50;
constexpr std::uint32_t NumLockKeycode = 77;
} // namespace

void EiKeyStateTests::initTestCase()
{
  m_arch.init();
  m_log.setFilter(LogLevel::Level::Verbose);
}

void EiKeyStateTests::clearStaleModifiers_shiftDownAndNumLockOn_shiftClearedAndNumLockPreserved()
{
  TestAppUtil appUtil;
  EventQueue eventQueue;
  deskflow::EiKeyState keyState(nullptr, &eventQueue);

  QTemporaryFile keymapFile;
  QVERIFY(keymapFile.open());
  const QByteArray keymapData = QByteArray::fromRawData(TestKeymap, sizeof(TestKeymap) - 1);
  QCOMPARE(keymapFile.write(keymapData), keymapData.size());
  QVERIFY(keymapFile.flush());
  keyState.init(keymapFile.handle(), keymapFile.size());

  keyState.updateXkbState(LeftShiftKeycode, true);
  keyState.updateXkbState(NumLockKeycode, true);
  keyState.updateXkbState(NumLockKeycode, false);

  QVERIFY((keyState.pollActiveModifiers() & KeyModifierShift) != 0);
  QVERIFY((keyState.pollActiveModifiers() & KeyModifierNumLock) != 0);

  keyState.clearStaleModifiers();

  QVERIFY((keyState.pollActiveModifiers() & KeyModifierShift) == 0);
  QVERIFY((keyState.pollActiveModifiers() & KeyModifierNumLock) != 0);
}

namespace {
/// Compiles the real de(neo_qwertz) keymap and writes it to a temp file for EiKeyState::init().
class NeoKeymapFile : public QTemporaryFile
{
public:
  NeoKeymapFile()
  {
    xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    xkb_rule_names rules{nullptr, nullptr, "de,de", "neo_qwertz,nodeadkeys", nullptr};
    xkb_keymap *km = xkb_keymap_new_from_names(ctx, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
    Q_ASSERT(km != nullptr);
    char *str = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
    Q_ASSERT(str != nullptr);
    QByteArray data = QByteArray::fromRawData(str, static_cast<int>(strlen(str)));
    Q_ASSERT(open());
    Q_ASSERT(write(data) == data.size());
    Q_ASSERT(flush());
    xkb_keymap_unref(km);
    xkb_context_unref(ctx);
    ::free(str);
  }
};

/// EiKeyState that records fakeKey() calls instead of touching the (absent) EiScreen.
class RecordingKeyState : public deskflow::EiKeyState
{
public:
  using deskflow::EiKeyState::EiKeyState;
  std::vector<std::pair<std::uint32_t, bool>> fakes;

protected:
  void fakeKey(const Keystroke &keystroke) override
  {
    if (keystroke.m_type == Keystroke::KeyType::Button) {
      fakes.emplace_back(keystroke.m_data.m_button.m_button, keystroke.m_data.m_button.m_press);
    }
  }
};

// evdev keycodes (== deskflow KeyButtons in the Ei backend)
constexpr std::uint32_t CapsButton = 58; // KEY_CAPSLOCK
constexpr std::uint32_t SButton = 31;    // KEY_S
} // namespace

void EiKeyStateTests::neoQwertz_serverSide_capsIsLevel3Shift_sIsSlash()
{
  TestAppUtil appUtil;
  EventQueue eventQueue;
  NeoKeymapFile keymap;

  deskflow::EiKeyState server(nullptr, &eventQueue);
  server.init(keymap.handle(), keymap.size());
  server.updateKeyMap();

  // CAPS in de(neo_qwertz) is ISO_Level3_Shift -> kKeyAltGr
  QCOMPARE(static_cast<KeyID>(server.mapKeyFromKeyval(CapsButton + 8)), static_cast<KeyID>(kKeyAltGr));

  server.updateXkbState(CapsButton + 8, true);
  const KeyModifierMask capsMask = server.pollActiveModifiers();
  QVERIFY((capsMask & KeyModifierAltGr) != 0);

  // CAPS + S -> slash
  QCOMPARE(static_cast<KeyID>(server.mapKeyFromKeyval(SButton + 8)), static_cast<KeyID>(0x2f));
}

void EiKeyStateTests::neoQwertz_clientSide_capsAndS_injectsCapsAndSButtons()
{
  TestAppUtil appUtil;
  EventQueue eventQueue;
  NeoKeymapFile keymap;

  RecordingKeyState client(nullptr, &eventQueue);
  client.init(keymap.handle(), keymap.size());
  client.updateKeyMap();

  const KeyModifierMask capsMask = KeyModifierAltGr;

  // Realistic: user first types a plain 's', THEN tries CAPS+S.
  client.fakeKeyDown(0x73, 0, SButton + 8, "");
  client.fakeKeyUp(SButton + 8);

  // Server relayed: CAPS (as kKeyAltGr) down, then S (as kKeySlash) down, then releases.
  client.fakeKeyDown(kKeyAltGr, capsMask, CapsButton + 8, "");
  client.fakeKeyDown(0x2f, capsMask, SButton + 8, "");
  client.fakeKeyUp(SButton + 8);
  client.fakeKeyUp(CapsButton + 8);

  bool capsPressed = false;
  bool sPressed = false;
  for (const auto &[button, press] : client.fakes) {
    if (button == CapsButton && press) {
      capsPressed = true;
    }
    if (button == SButton && press) {
      sPressed = true;
    }
  }
  QVERIFY2(capsPressed, "client never pressed the CAPS button");
  QVERIFY2(sPressed, "client never pressed the S button");
}

void EiKeyStateTests::updateLockedModifiers_compositorLockState_numLockFollowsCompositor()
{
  // Real modifier Mod2 is index 4 (Shift, Lock, Control, Mod1, Mod2, ...).
  constexpr std::uint32_t xkbMod2Mask = 1 << 4;

  TestAppUtil appUtil;
  EventQueue eventQueue;
  deskflow::EiKeyState keyState(nullptr, &eventQueue);

  QTemporaryFile keymapFile;
  QVERIFY(keymapFile.open());
  const QByteArray keymapData = QByteArray::fromRawData(TestKeymap, sizeof(TestKeymap) - 1);
  QCOMPARE(keymapFile.write(keymapData), keymapData.size());
  QVERIFY(keymapFile.flush());
  keyState.init(keymapFile.handle(), keymapFile.size());

  keyState.updateXkbState(LeftShiftKeycode, true);
  QVERIFY((keyState.pollActiveModifiers() & KeyModifierNumLock) == 0);

  // NumLock toggled on locally, reported by the compositor
  keyState.updateLockedModifiers(xkbMod2Mask);
  QVERIFY((keyState.pollActiveModifiers() & KeyModifierNumLock) != 0);
  QVERIFY((keyState.pollActiveModifiers() & KeyModifierShift) != 0);

  keyState.clearStaleModifiers();
  QVERIFY((keyState.pollActiveModifiers() & KeyModifierNumLock) != 0);

  // NumLock toggled off locally
  keyState.updateLockedModifiers(0);
  QVERIFY((keyState.pollActiveModifiers() & KeyModifierNumLock) == 0);
}

QTEST_MAIN(EiKeyStateTests)
