/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "arch/Arch.h"
#include "base/Log.h"

#include <QTest>

class EiKeyStateTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void initTestCase();
  void clearStaleModifiers_shiftDownAndNumLockOn_shiftClearedAndNumLockPreserved();
  void neoQwertz_serverSide_capsIsLevel3Shift_sIsSlash();
  void neoQwertz_clientSide_capsAndS_injectsCapsAndSButtons();
  void neoQwertz_serverSide_capsAndX_isDollar();
  void neoQwertz_clientSide_capsAndX_fakesDollar();
  void neoQwertz_clientSide_roundTripsNeoLayers();
  void updateLockedModifiers_compositorLockState_numLockFollowsCompositor();

private:
  Arch m_arch;
  Log m_log;
};
