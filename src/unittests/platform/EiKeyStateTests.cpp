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
/// Note: no Q_ASSERT here - they compile away in release builds and would leave
/// the keymap file unopened (the failure then shows up as a bogus test failure).
class NeoKeymapFile : public QTemporaryFile
{
public:
  NeoKeymapFile()
  {
    xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    xkb_rule_names rules{nullptr, nullptr, "de,de", "neo_qwertz,nodeadkeys", nullptr};
    xkb_keymap *km = ctx ? xkb_keymap_new_from_names(ctx, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS) : nullptr;
    char *str = km ? xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1) : nullptr;
    if (str == nullptr) {
      qFatal("could not compile the de(neo_qwertz) keymap - is xkeyboard-config available?");
    }

    const QByteArray data = QByteArray::fromRawData(str, static_cast<int>(strlen(str)));
    const bool written = open() && write(data) == data.size() && flush();

    xkb_keymap_unref(km);
    xkb_context_unref(ctx);
    ::free(str);

    if (!written) {
      qFatal("could not write the de(neo_qwertz) keymap to a temporary file");
    }
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
constexpr std::uint32_t CapsButton = 58; // KEY_CAPSLOCK (ISO_Level3_Shift in de(neo*))
constexpr std::uint32_t ShiftLeft = 42;  // KEY_LEFTSHIFT
constexpr std::uint32_t SButton = 31;    // KEY_S
constexpr std::uint32_t XButton = 45;    // KEY_X

QString keysymName(xkb_keysym_t keysym)
{
  char name[128] = {0};
  xkb_keysym_get_name(keysym, name, sizeof(name));
  return QString::fromUtf8(name);
}

/// Replays buttons through a fresh de(neo_qwertz) xkb state to find out which
/// symbols injected keystrokes actually generate.
class KeySymReplay
{
public:
  KeySymReplay()
  {
    m_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    xkb_rule_names rules{nullptr, nullptr, "de,de", "neo_qwertz,nodeadkeys", nullptr};
    m_keymap = xkb_keymap_new_from_names(m_ctx, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (m_keymap == nullptr) {
      qFatal("could not compile the de(neo_qwertz) keymap - is xkeyboard-config available?");
    }
    m_state = xkb_state_new(m_keymap);
  }

  ~KeySymReplay()
  {
    xkb_state_unref(m_state);
    xkb_keymap_unref(m_keymap);
    xkb_context_unref(m_ctx);
  }

  KeySymReplay(const KeySymReplay &) = delete;
  KeySymReplay &operator=(const KeySymReplay &) = delete;

  void press(std::uint32_t evdevCode)
  {
    xkb_state_update_key(m_state, evdevCode + 8, XKB_KEY_DOWN);
    m_presses.push_back(xkb_state_key_get_one_sym(m_state, evdevCode + 8));
  }

  void release(std::uint32_t evdevCode)
  {
    xkb_state_update_key(m_state, evdevCode + 8, XKB_KEY_UP);
  }

  //! The keysym generated by the last press
  xkb_keysym_t lastSym() const
  {
    return m_presses.empty() ? 0 : m_presses.back();
  }

  //! e.g. "x ISO_Level3_Shift dollar", for use in assertion messages
  QString symbolSummary() const
  {
    QString summary;
    for (const auto keysym : m_presses) {
      summary += keysymName(keysym) + " ";
    }
    return summary;
  }

  //! The keysym xkb generates for \p key with \p level3Key and \p shiftKey held
  /*! This is the reference the injected keystrokes are compared against. */
  xkb_keysym_t expectedSymbol(std::uint32_t level3Key, std::uint32_t shiftKey, std::uint32_t key) const
  {
    auto *state = xkb_state_new(m_keymap);
    for (const auto code : {level3Key, shiftKey, key}) {
      if (code != 0) {
        xkb_state_update_key(state, code + 8, XKB_KEY_DOWN);
      }
    }
    const auto keysym = xkb_state_key_get_one_sym(state, key + 8);
    xkb_state_unref(state);
    return keysym;
  }

private:
  xkb_context *m_ctx = nullptr;
  xkb_keymap *m_keymap = nullptr;
  xkb_state *m_state = nullptr;
  std::vector<xkb_keysym_t> m_presses;
};
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

void EiKeyStateTests::neoQwertz_serverSide_capsAndX_isDollar()
{
  TestAppUtil appUtil;
  EventQueue eventQueue;
  NeoKeymapFile keymap;

  deskflow::EiKeyState server(nullptr, &eventQueue);
  server.init(keymap.handle(), keymap.size());
  server.updateKeyMap();

  server.updateXkbState(CapsButton + 8, true);

  // CAPS + X -> dollar
  QCOMPARE(static_cast<KeyID>(server.mapKeyFromKeyval(XButton + 8)), static_cast<KeyID>(0x24));
}

void EiKeyStateTests::neoQwertz_clientSide_capsAndX_fakesDollar()
{
  TestAppUtil appUtil;
  EventQueue eventQueue;
  NeoKeymapFile keymap;

  RecordingKeyState client(nullptr, &eventQueue);
  client.init(keymap.handle(), keymap.size());
  client.updateKeyMap();

  const KeyModifierMask capsMask = KeyModifierAltGr;

  // The remote holds CAPS (ISO_Level3_Shift in this layout) and presses X, which
  // gives '$'.  Note that '$' also exists elsewhere in the layout (Shift+6) and
  // that same 6 key generates 'cent' at Ebene 3, so an entry picked by chance
  // rather than by the key the remote actually pressed injects 'cent'.
  client.fakeKeyDown(0x78, 0, XButton + 8, "");
  client.fakeKeyUp(XButton + 8);

  client.fakeKeyDown(kKeyAltGr, capsMask, CapsButton + 8, "");
  client.fakeKeyDown(0x24, capsMask, XButton + 8, "");

  KeySymReplay replay;
  for (const auto &[button, press] : client.fakes) {
    if (press) {
      replay.press(button);
    } else {
      replay.release(button);
    }
  }

  QVERIFY2(replay.lastSym() == 0x24, qPrintable(QString("injected: %1").arg(replay.symbolSummary())));
}

void EiKeyStateTests::neoQwertz_clientSide_roundTripsNeoLayers()
{
  TestAppUtil appUtil;
  EventQueue eventQueue;
  NeoKeymapFile keymap;

  deskflow::EiKeyState server(nullptr, &eventQueue);
  server.init(keymap.handle(), keymap.size());
  server.updateKeyMap();

  RecordingKeyState client(nullptr, &eventQueue);
  client.init(keymap.handle(), keymap.size());
  client.updateKeyMap();

  // evdev codes of the alphanumeric and punctuation keys, as the remote presses them
  static const std::vector<std::uint32_t> keys = {
      2,  3,  4,  5,  6,  7,  8,  9,  10, 11, // 1..0
      16, 17, 18, 19, 20, 21, 22, 23, 24, 25, // q..p
      30, 31, 32, 33, 34, 35, 36, 37, 38,     // a..l
      44, 45, 46, 47, 48, 49, 50,             // y..m
      39, 40, 51, 52, 53                      // ; ' , . /
  };

  KeySymReplay reference;
  QString failures;
  for (const std::uint32_t level3Key : {0u, CapsButton}) {
    for (const std::uint32_t shiftKey : {0u, ShiftLeft}) {
      for (const std::uint32_t key : keys) {
        // what the server puts on the wire for this combination
        for (const std::uint32_t modifier : {level3Key, shiftKey}) {
          if (modifier != 0) {
            server.updateXkbState(modifier + 8, true);
          }
        }
        const KeyID keyID = server.mapKeyFromKeyval(key + 8);
        const KeyModifierMask mask = server.pollActiveModifiers();
        for (const std::uint32_t modifier : {level3Key, shiftKey}) {
          if (modifier != 0) {
            server.updateXkbState(modifier + 8, false);
          }
        }

        // what the client injects for it
        if (level3Key != 0) {
          client.fakeKeyDown(kKeyAltGr, mask, level3Key + 8, "");
        }
        if (shiftKey != 0) {
          client.fakeKeyDown(kKeyShift_L, mask, shiftKey + 8, "");
        }
        client.fakeKeyDown(keyID, mask, key + 8, "");
        const std::vector<std::pair<std::uint32_t, bool>> fakes = client.fakes;
        client.fakes.clear();
        client.fakeKeyUp(key + 8);
        if (shiftKey != 0) {
          client.fakeKeyUp(shiftKey + 8);
        }
        if (level3Key != 0) {
          client.fakeKeyUp(level3Key + 8);
        }

        KeySymReplay injected;
        for (const auto &[button, press] : fakes) {
          if (press) {
            injected.press(button);
          } else {
            injected.release(button);
          }
        }

        const xkb_keysym_t expected = reference.expectedSymbol(level3Key, shiftKey, key);
        if (keyID == 0 || injected.lastSym() == expected) {
          continue;
        }
        failures += QString("CAPS=%1 SHIFT=%2 KEY=%3 (keyID=0x%4 mask=0x%5): expected %6, injected %7\n")
                        .arg(level3Key)
                        .arg(shiftKey)
                        .arg(key)
                        .arg(keyID, 0, 16)
                        .arg(mask, 0, 16)
                        .arg(keysymName(expected))
                        .arg(injected.symbolSummary());
      }
    }
  }
  QVERIFY2(failures.isEmpty(), qPrintable(failures));
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
