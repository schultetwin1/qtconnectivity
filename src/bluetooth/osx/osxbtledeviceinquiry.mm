/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtBluetooth module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "osxbtledeviceinquiry_p.h"
#include "qbluetoothdeviceinfo.h"
#include "osxbtnotifier_p.h"
#include "qbluetoothuuid.h"
#include "osxbtutility_p.h"

#include <QtCore/qloggingcategory.h>
#include <QtCore/qdebug.h>

#include "corebluetoothwrapper_p.h"

#include <algorithm>

QT_BEGIN_NAMESPACE

namespace OSXBluetooth {

QBluetoothUuid qt_uuid(NSUUID *nsUuid)
{
    if (!nsUuid)
        return QBluetoothUuid();

    uuid_t uuidData = {};
    [nsUuid getUUIDBytes:uuidData];
    quint128 qtUuidData = {};
    std::copy(uuidData, uuidData + 16, qtUuidData.data);
    return QBluetoothUuid(qtUuidData);
}

const int timeStepMS = 100;

const int powerOffTimeoutMS = 30000;
const qreal powerOffTimeStepS = 30. / 100.;

}

QT_END_NAMESPACE

QT_USE_NAMESPACE

@interface QT_MANGLE_NAMESPACE(OSXBTLEDeviceInquiry) (PrivateAPI) <CBCentralManagerDelegate>
// These two methods are scheduled with a small time step
// within a given timeout, they either re-schedule
// themselves or emit a signal/stop some operation.
- (void)stopScan;
- (void)handlePoweredOff;
@end

@implementation QT_MANGLE_NAMESPACE(OSXBTLEDeviceInquiry)

-(id)initWithNotifier:(LECBManagerNotifier *)aNotifier
{
    if (self = [super init]) {
        Q_ASSERT(aNotifier);
        notifier = aNotifier;
        uuids.reset([[NSMutableSet alloc] init]);
        internalState = InquiryStarting;
        inquiryTimeoutMS = OSXBluetooth::defaultLEScanTimeoutMS;
    }

    return self;
}

- (void)dealloc
{
    if (manager) {
        [manager setDelegate:nil];
        if (internalState == InquiryActive)
            [manager stopScan];
    }

    if (notifier) {
        notifier->disconnect();
        notifier->deleteLater();
    }

    [super dealloc];
}

- (void)stopScan
{
    using namespace OSXBluetooth;

    // We never schedule stopScan if there is no timeout:
    Q_ASSERT(inquiryTimeoutMS > 0);

    if (internalState == InquiryActive) {
        const int elapsed = scanTimer.elapsed();
        if (elapsed >= inquiryTimeoutMS) {
            [manager stopScan];
            [manager setDelegate:nil];
            internalState = InquiryFinished;
            Q_ASSERT(notifier);
            emit notifier->discoveryFinished();
        } else {
            // Re-schedule 'stopScan':
            dispatch_queue_t leQueue(qt_LE_queue());
            Q_ASSERT(leQueue);
            const int timeChunkMS = std::min(inquiryTimeoutMS - elapsed, timeStepMS);
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                         int64_t(timeChunkMS / 1000. * NSEC_PER_SEC)),
                                         leQueue,
                                         ^{
                                               [self stopScan];
                                          });
        }
    }
}

- (void)handlePoweredOff
{
    // This is interesting on iOS only, where
    // the system shows an alert asking to enable
    // Bluetooth in the 'Settings' app. If not done yet (after 30
    // seconds) - we consider it an error.
    using namespace OSXBluetooth;

    if (internalState == InquiryStarting) {
        if (errorTimer.elapsed() >= powerOffTimeoutMS) {
            [manager setDelegate:nil];
            internalState = ErrorPoweredOff;
            Q_ASSERT(notifier);
            emit notifier->CBManagerError(QBluetoothDeviceDiscoveryAgent::PoweredOffError);
        } else {
            dispatch_queue_t leQueue(qt_LE_queue());
            Q_ASSERT(leQueue);
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                         (int64_t)(powerOffTimeStepS * NSEC_PER_SEC)),
                                         leQueue,
                                         ^{
                                              [self handlePoweredOff];
                                          });
        }
    }
}

- (void)startWithTimeout:(int)timeout
{
    dispatch_queue_t leQueue(OSXBluetooth::qt_LE_queue());
    Q_ASSERT(leQueue);
    inquiryTimeoutMS = timeout;
    manager.reset([[CBCentralManager alloc] initWithDelegate:self queue:leQueue]);
}

- (void)centralManagerDidUpdateState:(CBCentralManager *)central
{
    if (central != manager)
        return;

    if (internalState != InquiryActive && internalState != InquiryStarting)
        return;

    Q_ASSERT(notifier);

    using namespace OSXBluetooth;

    dispatch_queue_t leQueue(qt_LE_queue());
    Q_ASSERT(leQueue);

    const CBCentralManagerState cbState(central.state);
    if (cbState == CBCentralManagerStatePoweredOn) {
        if (internalState == InquiryStarting) {
            internalState = InquiryActive;

            if (inquiryTimeoutMS > 0) {
                // We have a finite-length discovery, schedule stopScan,
                // with a smaller time step, otherwise it can prevent
                // 'self' from being deleted in time, which is not good
                // (the block will retain 'self', waiting for timeout).
                scanTimer.start();
                const int timeChunkMS = std::min(timeStepMS, inquiryTimeoutMS);
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                             int64_t(timeChunkMS / 1000. * NSEC_PER_SEC)),
                                             leQueue,
                                             ^{
                                                   [self stopScan];
                                              });
            }

            [manager scanForPeripheralsWithServices:nil options:nil];
        } // Else we ignore.
    } else if (cbState == CBCentralManagerStateUnsupported
               || cbState == CBCentralManagerStateUnauthorized) {
        if (internalState == InquiryActive) {
            [manager stopScan];
            // Not sure how this is possible at all,
            // probably, can never happen.
            internalState = ErrorPoweredOff;
            emit notifier->CBManagerError(QBluetoothDeviceDiscoveryAgent::PoweredOffError);
        } else {
            internalState = ErrorLENotSupported;
            emit notifier->LEnotSupported();
        }

        [manager setDelegate:nil];
    } else if (cbState == CBCentralManagerStatePoweredOff) {
        if (internalState == InquiryStarting) {
#ifndef Q_OS_OSX
            // On iOS a user can see at this point an alert asking to
            // enable Bluetooth in the "Settings" app. If a user does,
            // we'll receive 'PoweredOn' state update later.
            // No change in internalState. Wait for 30 seconds
            // (we split it into smaller steps not to retain 'self' for
            // too long ) ...
            errorTimer.start();
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                         (int64_t)(powerOffTimeStepS * NSEC_PER_SEC)),
                                         leQueue,
                                         ^{
                                              [self handlePoweredOff];
                                          });
            return;
#endif
            internalState = ErrorPoweredOff;
            emit notifier->CBManagerError(QBluetoothDeviceDiscoveryAgent::PoweredOffError);
        } else {
            [manager stopScan];
            emit notifier->CBManagerError(QBluetoothDeviceDiscoveryAgent::PoweredOffError);
        }

        [manager setDelegate:nil];
    } else {
        // The following two states we ignore (from Apple's docs):
        //"
        // -CBCentralManagerStateUnknown
        // The current state of the central manager is unknown;
        // an update is imminent.
        //
        // -CBCentralManagerStateResetting
        // The connection with the system service was momentarily
        // lost; an update is imminent. "
        // Wait for this imminent update.
    }
}

- (void)stop
{
    if (internalState == InquiryActive)
        [manager stopScan];

    [manager setDelegate:nil];
    internalState = InquiryCancelled;

    notifier->disconnect();
    notifier->deleteLater();
    notifier = nullptr;
}

- (void)centralManager:(CBCentralManager *)central
        didDiscoverPeripheral:(CBPeripheral *)peripheral
        advertisementData:(NSDictionary *)advertisementData
        RSSI:(NSNumber *)RSSI
{
    Q_UNUSED(advertisementData);

    using namespace OSXBluetooth;

    if (central != manager)
        return;

    if (internalState != InquiryActive)
        return;

    if (!notifier)
        return;

    QBluetoothUuid deviceUuid;

    if (!peripheral.identifier) {
        qCWarning(QT_BT_OSX) << "peripheral without NSUUID";
        return;
    }

    if ([uuids containsObject:peripheral.identifier]) {
        // TODO: my understanding of the same peripheral reported many times seems
        // to be outdated or even wrong - nowadays it's reported twice and the
        // second time (AFAIK) more info can be extracted ...
        return;
    }

    [uuids addObject:peripheral.identifier];
    deviceUuid = OSXBluetooth::qt_uuid(peripheral.identifier);

    if (deviceUuid.isNull()) {
        qCWarning(QT_BT_OSX) << "no way to address peripheral, QBluetoothUuid is null";
        return;
    }

    QString name;
    if (peripheral.name)
        name = QString::fromNSString(peripheral.name);

    // TODO: fix 'classOfDevice' (0 for now).
    QBluetoothDeviceInfo newDeviceInfo(deviceUuid, name, 0);
    if (RSSI)
        newDeviceInfo.setRssi([RSSI shortValue]);
    // CoreBluetooth scans only for LE devices.
    newDeviceInfo.setCoreConfigurations(QBluetoothDeviceInfo::LowEnergyCoreConfiguration);
    emit notifier->deviceDiscovered(newDeviceInfo);
}

@end
