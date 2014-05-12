/****************************************************************************
**
** Copyright (C) 2014 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the QtBluetooth module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qbluetoothservicediscoveryagent.h"
#include "qbluetoothservicediscoveryagent_p.h"

#include "bluez/manager_p.h"
#include "bluez/adapter_p.h"
#include "bluez/device_p.h"
#include "bluez/bluez5_helper_p.h"
#include "bluez/objectmanager_p.h"
#include "bluez/adapter1_bluez5_p.h"

#include "bluetooth/bluetooth.h"
#include "bluetooth/sdp.h"
#include "bluetooth/sdp_lib.h"

#include <QtCore/QLoggingCategory>
#include <QtDBus/QDBusPendingCallWatcher>
#include <QtConcurrent/QtConcurrentRun>

QT_BEGIN_NAMESPACE

Q_DECLARE_LOGGING_CATEGORY(QT_BT_BLUEZ)

static inline void convertAddress(quint64 from, quint8 (&to)[6])
{
    to[0] = (from >> 0) & 0xff;
    to[1] = (from >> 8) & 0xff;
    to[2] = (from >> 16) & 0xff;
    to[3] = (from >> 24) & 0xff;
    to[4] = (from >> 32) & 0xff;
    to[5] = (from >> 40) & 0xff;
}

QBluetoothServiceDiscoveryAgentPrivate::QBluetoothServiceDiscoveryAgentPrivate(const QBluetoothAddress &deviceAdapter)
:   error(QBluetoothServiceDiscoveryAgent::NoError), m_deviceAdapterAddress(deviceAdapter), state(Inactive), deviceDiscoveryAgent(0),
    mode(QBluetoothServiceDiscoveryAgent::MinimalDiscovery), singleDevice(false),
    manager(0), managerBluez5(0), adapter(0), device(0)
{
    if (isBluez5()) {
        managerBluez5 = new OrgFreedesktopDBusObjectManagerInterface(
                                    QStringLiteral("org.bluez"), QStringLiteral("/"),
                                    QDBusConnection::systemBus());
        qRegisterMetaType<QBluetoothServiceDiscoveryAgent::Error>("QBluetoothServiceDiscoveryAgent::Error");
    } else {
        qRegisterMetaType<ServiceMap>("ServiceMap");
        qDBusRegisterMetaType<ServiceMap>();

        manager = new OrgBluezManagerInterface(QStringLiteral("org.bluez"), QStringLiteral("/"),
                                               QDBusConnection::systemBus());
    }
}

QBluetoothServiceDiscoveryAgentPrivate::~QBluetoothServiceDiscoveryAgentPrivate()
{
    delete device;
    delete manager;
    delete managerBluez5;
    delete adapter;
}

void QBluetoothServiceDiscoveryAgentPrivate::start(const QBluetoothAddress &address)
{
    Q_Q(QBluetoothServiceDiscoveryAgent);

    qCDebug(QT_BT_BLUEZ) << "Discovery on: " << address.toString() << "Mode:" << DiscoveryMode();

    if (managerBluez5) {
        startBluez5(address);
        return;
    }

    QDBusPendingReply<QDBusObjectPath> reply;
    if (m_deviceAdapterAddress.isNull())
        reply = manager->DefaultAdapter();
    else
        reply = manager->FindAdapter(m_deviceAdapterAddress.toString());

    reply.waitForFinished();
    if (reply.isError()) {
        error = QBluetoothServiceDiscoveryAgent::InputOutputError;
        errorString = QBluetoothServiceDiscoveryAgent::tr("Unable to find appointed local adapter");
        emit q->error(error);
        _q_serviceDiscoveryFinished();
        return;
    }

    adapter = new OrgBluezAdapterInterface(QStringLiteral("org.bluez"), reply.value().path(),
                                           QDBusConnection::systemBus());

    QDBusPendingReply<QDBusObjectPath> deviceObjectPath = adapter->CreateDevice(address.toString());

    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(deviceObjectPath, q);
    watcher->setProperty("_q_BTaddress", QVariant::fromValue(address));
    QObject::connect(watcher, SIGNAL(finished(QDBusPendingCallWatcher*)),
                     q, SLOT(_q_createdDevice(QDBusPendingCallWatcher*)));

}

void QBluetoothServiceDiscoveryAgentPrivate::startBluez5(const QBluetoothAddress &address)
{
    Q_Q(QBluetoothServiceDiscoveryAgent);

    if (foundHostAdapterPath.isEmpty()) {
        // check that we match adapter addresses or use first if it wasn't specified

        QDBusPendingReply<ManagedObjectList> reply = managerBluez5->GetManagedObjects();
        reply.waitForFinished();
        if (reply.isError()) {
            discoveredDevices.clear();
            error = QBluetoothServiceDiscoveryAgent::InputOutputError;
            errorString = reply.error().message();
            emit q->error(error);
            _q_serviceDiscoveryFinished();
            return;
        }

        const QString desiredAdapter = m_deviceAdapterAddress.toString();
        foreach (const QDBusObjectPath &path, reply.value().keys()) {
            const InterfaceList ifaceList = reply.value().value(path);
            foreach (const QString &iface, ifaceList.keys()) {
                if (iface == QStringLiteral("org.bluez.Adapter1")) {
                    if (m_deviceAdapterAddress.isNull()
                        || desiredAdapter == ifaceList.value(iface).
                                value(QStringLiteral("Address")).toString()) {
                        // use first adapter or we just matched one
                        foundHostAdapterPath = path.path();
                    }

                    if (!foundHostAdapterPath.isEmpty())
                        break;
                }
            }

            if (!foundHostAdapterPath.isEmpty())
                break;
        }

        if (foundHostAdapterPath.isEmpty()) {
            // Cannot find a local adapter
            // Abort any outstanding discoveries
            discoveredDevices.clear();

            error = QBluetoothServiceDiscoveryAgent::InvalidBluetoothAdapterError;
            errorString = QBluetoothServiceDiscoveryAgent::tr("Cannot find local Bluetooth adapter");
            emit q->error(error);
            _q_serviceDiscoveryFinished();

            return;
        }
    }

    // ensure we didn't go offline yet
    OrgBluezAdapter1Interface adapter(QStringLiteral("org.bluez"),
                                      foundHostAdapterPath, QDBusConnection::systemBus());
    if (!adapter.powered()) {
        discoveredDevices.clear();

        error = QBluetoothServiceDiscoveryAgent::PoweredOffError;
        errorString = QBluetoothServiceDiscoveryAgent::tr("Local device is powered off");
        emit q->error(error);

        _q_serviceDiscoveryFinished();
        return;
    }

    if (DiscoveryMode() == QBluetoothServiceDiscoveryAgent::MinimalDiscovery) {
        performMinimalServiceDiscovery(address);
    } else {
        // we need to run the discovery in a different thread
        // as it involves blocking calls
        QtConcurrent::run(this, &QBluetoothServiceDiscoveryAgentPrivate::runSdpScan,
                          address, QBluetoothAddress(adapter.address()));
    }
}

/*
 * This function runs in a different thread. We need to be very careful what we
 * access from here. That's why invokeMethod is used below.
 */
void QBluetoothServiceDiscoveryAgentPrivate::runSdpScan(
        const QBluetoothAddress &remoteAddress, const QBluetoothAddress localAddress)
{
    Q_Q(QBluetoothServiceDiscoveryAgent);

    // connect to SDP server
    bdaddr_t local, remote;
    convertAddress(localAddress.toUInt64(), local.b);
    convertAddress(remoteAddress.toUInt64(), remote.b);

    /* We use singleshot timer below because this function runs in a different
     * thread than the rest of this class.
     */

    sdp_session_t *session = sdp_connect( &local, &remote, SDP_RETRY_IF_BUSY);
    // try one more time if first attempt fails
    if (!session)
        session = sdp_connect( &local, &remote, SDP_RETRY_IF_BUSY);

    qCDebug(QT_BT_BLUEZ) << "SDP for" << remoteAddress.toString() << session << qt_error_string(errno);
    if (!session) {
        if (singleDevice) {
            // was sole device without result -> error
            QMetaObject::invokeMethod(q, "_q_finishSdpScan", Qt::QueuedConnection,
                                  Q_ARG(QBluetoothServiceDiscoveryAgent::Error,
                                        QBluetoothServiceDiscoveryAgent::InputOutputError),
                                  Q_ARG(QString,
                                        QBluetoothServiceDiscoveryAgent::tr("Unable to access device")),
                                  Q_ARG(QStringList, QStringList()));
        } else {
            // go to next device
            QMetaObject::invokeMethod(q, "_q_finishSdpScan", Qt::QueuedConnection,
                                      Q_ARG(QBluetoothServiceDiscoveryAgent::Error,
                                            QBluetoothServiceDiscoveryAgent::NoError),
                                      Q_ARG(QString, QString()),
                                      Q_ARG(QStringList, QStringList()));
        }

        return;
    }


    // set the filter for service matches
    uuid_t publicBrowseGroupUuid;
    sdp_uuid16_create(&publicBrowseGroupUuid, QBluetoothUuid::PublicBrowseGroup);
    sdp_list_t *serviceFilter;
    serviceFilter = sdp_list_append(0, &publicBrowseGroupUuid);

    uint32_t attributeRange = 0x0000ffff; //all attributes
    sdp_list_t *attributes;
    attributes = sdp_list_append(0, &attributeRange);

    sdp_list_t* sdpResults;
    int result = sdp_service_search_attr_req(session, serviceFilter, SDP_ATTR_REQ_RANGE,
                                             attributes, &sdpResults);
    sdp_list_free(attributes, 0);
    sdp_list_free(serviceFilter, 0);

    if (result != 0) {
        qCDebug(QT_BT_BLUEZ) << "SDP search failed" << qt_error_string(errno);
        sdp_close(session);
        if (singleDevice) {
            QMetaObject::invokeMethod(q, "_q_finishSdpScan", Qt::QueuedConnection,
                                  Q_ARG(QBluetoothServiceDiscoveryAgent::Error,
                                        QBluetoothServiceDiscoveryAgent::InputOutputError),
                                  Q_ARG(QString,
                                        QBluetoothServiceDiscoveryAgent::tr("Unable to access device")),
                                  Q_ARG(QStringList, QStringList()));
        } else {
            QMetaObject::invokeMethod(q, "_q_finishSdpScan", Qt::QueuedConnection,
                                      Q_ARG(QBluetoothServiceDiscoveryAgent::Error,
                                            QBluetoothServiceDiscoveryAgent::NoError),
                                      Q_ARG(QString, QString()),
                                      Q_ARG(QStringList, QStringList()));
        }
        return;
    }

    qCDebug(QT_BT_BLUEZ) << "SDP search a success. Iterating results" << sdpResults;
    QStringList xmlRecords;

    // process the results
    for ( ; sdpResults; sdpResults = sdpResults->next) {
        sdp_record_t *record = (sdp_record_t *) sdpResults->data;

        QByteArray xml = parseSdpRecord(record);
        if (xml.isEmpty())
            continue;

        //qDebug() << xml;
        xmlRecords.append(QString::fromUtf8(xml));
    }

    sdp_close(session);

    QMetaObject::invokeMethod(q, "_q_finishSdpScan", Qt::QueuedConnection,
                              Q_ARG(QBluetoothServiceDiscoveryAgent::Error,
                                    QBluetoothServiceDiscoveryAgent::NoError),
                              Q_ARG(QString, QString()),
                              Q_ARG(QStringList, xmlRecords));
}

void QBluetoothServiceDiscoveryAgentPrivate::_q_finishSdpScan(QBluetoothServiceDiscoveryAgent::Error errorCode,
                                                              const QString &errorDescription,
                                                              const QStringList &xmlRecords)
{
    Q_Q(QBluetoothServiceDiscoveryAgent);

    if (errorCode != QBluetoothServiceDiscoveryAgent::NoError) {
        qCWarning(QT_BT_BLUEZ) << "SDP search failed for"
                              << discoveredDevices.at(0).address().toString();
        // We have an error which we need to indicate and stop further processing
        discoveredDevices.clear();
        error = errorCode;
        errorString = errorDescription;
        emit q->error(error);
    } else if (!xmlRecords.isEmpty() && discoveryState() != Inactive) {
        foreach (const QString &record, xmlRecords) {
            const QBluetoothServiceInfo serviceInfo = parseServiceXml(record);

            //apply uuidFilter
            if (!uuidFilter.isEmpty()) {
                bool serviceNameMatched = uuidFilter.contains(serviceInfo.serviceUuid());
                bool serviceClassMatched = false;
                foreach (const QBluetoothUuid &id, serviceInfo.serviceClassUuids()) {
                    if (uuidFilter.contains(id)) {
                        serviceClassMatched = true;
                        break;
                    }
                }

                if (!serviceNameMatched && !serviceClassMatched)
                    continue;
            }

            if (!serviceInfo.isValid())
                continue;

            if (!isDuplicatedService(serviceInfo)) {
                discoveredServices.append(serviceInfo);
                qCDebug(QT_BT_BLUEZ) << "Discovered services" << discoveredDevices.at(0).address().toString()
                                     << serviceInfo.serviceName() << serviceInfo.serviceUuid()
                                     << ">>>" << serviceInfo.serviceClassUuids();

                emit q->serviceDiscovered(serviceInfo);
            }
        }
    }

    _q_serviceDiscoveryFinished();
}

void QBluetoothServiceDiscoveryAgentPrivate::stop()
{
    qCDebug(QT_BT_BLUEZ) << Q_FUNC_INFO << "Stop called";
    if (device) {
        //we are waiting for _q_discoveredServices() slot to be called
        // adapter is already 0
        QDBusPendingReply<> reply = device->CancelDiscovery();
        reply.waitForFinished();

        device->deleteLater();
        device = 0;
        Q_ASSERT(!adapter);
    } else if (adapter) {
        //we are waiting for _q_createdDevice() slot to be called
        adapter->deleteLater();
        adapter = 0;
        Q_ASSERT(!device);
    }

    discoveredDevices.clear();
    setDiscoveryState(Inactive);
    Q_Q(QBluetoothServiceDiscoveryAgent);
    emit q->canceled();
}

void QBluetoothServiceDiscoveryAgentPrivate::_q_createdDevice(QDBusPendingCallWatcher *watcher)
{
    if (!adapter)
        return;

    Q_Q(QBluetoothServiceDiscoveryAgent);

    const QBluetoothAddress &address = watcher->property("_q_BTaddress").value<QBluetoothAddress>();

    qCDebug(QT_BT_BLUEZ) << Q_FUNC_INFO << "created" << address.toString();

    QDBusPendingReply<QDBusObjectPath> deviceObjectPath = *watcher;
    if (deviceObjectPath.isError()) {
        if (deviceObjectPath.error().name() != QStringLiteral("org.bluez.Error.AlreadyExists")) {
            delete adapter;
            adapter = 0;
            _q_serviceDiscoveryFinished();
            qCDebug(QT_BT_BLUEZ) << "Create device failed Error: " << error << deviceObjectPath.error().name();
            return;
        }

        deviceObjectPath = adapter->FindDevice(address.toString());
        deviceObjectPath.waitForFinished();
        if (deviceObjectPath.isError()) {
            delete adapter;
            adapter = 0;
            if (singleDevice) {
                error = QBluetoothServiceDiscoveryAgent::InputOutputError;
                errorString = QBluetoothServiceDiscoveryAgent::tr("Unable to access device");
                emit q->error(error);
            }
            _q_serviceDiscoveryFinished();
            qCDebug(QT_BT_BLUEZ) << "Can't find device after creation Error: " << error << deviceObjectPath.error().name();
            return;
        }
    }

    device = new OrgBluezDeviceInterface(QStringLiteral("org.bluez"),
                                         deviceObjectPath.value().path(),
                                         QDBusConnection::systemBus());
    delete adapter;
    adapter = 0;

    QString pattern;
    foreach (const QBluetoothUuid &uuid, uuidFilter)
        pattern += uuid.toString().remove(QLatin1Char('{')).remove(QLatin1Char('}')) + QLatin1Char(' ');

    pattern = pattern.trimmed();
    qCDebug(QT_BT_BLUEZ) << Q_FUNC_INFO << "Discover restrictions:" << pattern;

    QDBusPendingReply<ServiceMap> discoverReply = device->DiscoverServices(pattern);
    watcher = new QDBusPendingCallWatcher(discoverReply, q);
    QObject::connect(watcher, SIGNAL(finished(QDBusPendingCallWatcher*)),
                     q, SLOT(_q_discoveredServices(QDBusPendingCallWatcher*)));
}

// Bluez 4
void QBluetoothServiceDiscoveryAgentPrivate::_q_discoveredServices(QDBusPendingCallWatcher *watcher)
{
    if (!device)
        return;

    qCDebug(QT_BT_BLUEZ) << Q_FUNC_INFO;

    QDBusPendingReply<ServiceMap> reply = *watcher;
    if (reply.isError()) {
        qCDebug(QT_BT_BLUEZ) << "discoveredServices error: " << error << reply.error().message();
        watcher->deleteLater();
        if (singleDevice) {
            Q_Q(QBluetoothServiceDiscoveryAgent);
            error = QBluetoothServiceDiscoveryAgent::UnknownError;
            errorString = reply.error().message();
            emit q->error(error);
        }
        delete device;
        device = 0;
        _q_serviceDiscoveryFinished();
        return;
    }

    ServiceMap map = reply.value();

    qCDebug(QT_BT_BLUEZ) << "Parsing xml" << discoveredDevices.at(0).address().toString() << discoveredDevices.count() << map.count();

    foreach (const QString &record, reply.value()) {
        const QBluetoothServiceInfo serviceInfo = parseServiceXml(record);

        if (!serviceInfo.isValid())
            return;

        // Don't need to apply uuidFilter because Bluez 4 applies
        // search pattern during DiscoverServices() call

        Q_Q(QBluetoothServiceDiscoveryAgent);

        if (!isDuplicatedService(serviceInfo)) {
            discoveredServices.append(serviceInfo);
            qCDebug(QT_BT_BLUEZ) << "Discovered services" << discoveredDevices.at(0).address().toString()
                                 << serviceInfo.serviceName();
            emit q->serviceDiscovered(serviceInfo);
        }

        // could stop discovery, check for state
        if (discoveryState() == Inactive)
            qCDebug(QT_BT_BLUEZ) << "Exit discovery after stop";
    }

    watcher->deleteLater();
    delete device;
    device = 0;

    _q_serviceDiscoveryFinished();
}

QBluetoothServiceInfo QBluetoothServiceDiscoveryAgentPrivate::parseServiceXml(const QString& xmlRecord)
{
    QXmlStreamReader xml(xmlRecord);

    QBluetoothServiceInfo serviceInfo;
    serviceInfo.setDevice(discoveredDevices.at(0));

    while (!xml.atEnd()) {
        xml.readNext();

        if (xml.tokenType() == QXmlStreamReader::StartElement &&
            xml.name() == QLatin1String("attribute")) {
            quint16 attributeId =
                xml.attributes().value(QLatin1String("id")).toString().toUShort(0, 0);

            if (xml.readNextStartElement()) {
                QVariant value = readAttributeValue(xml);

                serviceInfo.setAttribute(attributeId, value);
            }
        }
    }

    return serviceInfo;
}

void QBluetoothServiceDiscoveryAgentPrivate::performMinimalServiceDiscovery(const QBluetoothAddress &deviceAddress)
{
    if (foundHostAdapterPath.isEmpty()) {
        _q_serviceDiscoveryFinished();
        return;
    }

    Q_Q(QBluetoothServiceDiscoveryAgent);

    QDBusPendingReply<ManagedObjectList> reply = managerBluez5->GetManagedObjects();
    reply.waitForFinished();
    if (reply.isError()) {
        if (singleDevice) {
            error = QBluetoothServiceDiscoveryAgent::InputOutputError;
            errorString = reply.error().message();
            emit q->error(error);

        }
        _q_serviceDiscoveryFinished();
        return;
    }

    QStringList uuidStrings;
    foreach (const QDBusObjectPath &path, reply.value().keys()) {
        const InterfaceList ifaceList = reply.value().value(path);
        foreach (const QString &iface, ifaceList.keys()) {
            if (iface == QStringLiteral("org.bluez.Device1")) {
                const QVariantMap details = ifaceList.value(iface);
                if (deviceAddress.toString()
                        == details.value(QStringLiteral("Address")).toString()) {
                    uuidStrings = details.value(QStringLiteral("UUIDs")).toStringList();
                    break;

                }
            }
        }
        if (!uuidStrings.isEmpty())
            break;
    }

    if (uuidStrings.isEmpty() || discoveredDevices.isEmpty()) {
         // nothing found -> go to next uuid
        _q_serviceDiscoveryFinished();
        return;
    }

    qCDebug(QT_BT_BLUEZ) << "Minimal uuid list for" << deviceAddress.toString() << uuidStrings;

    QBluetoothUuid uuid;
    for (int i = 0; i < uuidStrings.count(); i++) {
        uuid = QBluetoothUuid(uuidStrings.at(i));
        if (uuid.isNull())
            continue;

        //apply uuidFilter
        if (!uuidFilter.isEmpty() && !uuidFilter.contains(uuid))
            continue;

        QBluetoothServiceInfo serviceInfo;
        serviceInfo.setDevice(discoveredDevices.at(0));

        if (uuid.minimumSize() == 16) { // not derived from Bluetooth Base UUID
            serviceInfo.setServiceUuid(uuid);
            serviceInfo.setServiceName(QBluetoothServiceDiscoveryAgent::tr("Custom Service"));
        } else {
            // set uuid as service class id
            QBluetoothServiceInfo::Sequence classId;
            classId << QVariant::fromValue(uuid);
            serviceInfo.setAttribute(QBluetoothServiceInfo::ServiceClassIds, classId);
            QBluetoothUuid::ServiceClassUuid clsId
                    = static_cast<QBluetoothUuid::ServiceClassUuid>(uuid.data1 & 0xffff);
            serviceInfo.setServiceName(QBluetoothUuid::serviceClassToString(clsId));
        }

        //don't include the service if we already discovered it before
        if (!isDuplicatedService(serviceInfo)) {
            discoveredServices << serviceInfo;
            qCDebug(QT_BT_BLUEZ) << "Discovered services" << discoveredDevices.at(0).address().toString()
                                 << serviceInfo.serviceName();
            emit q->serviceDiscovered(serviceInfo);
        }
    }

    _q_serviceDiscoveryFinished();
}

QVariant QBluetoothServiceDiscoveryAgentPrivate::readAttributeValue(QXmlStreamReader &xml)
{
    if (xml.name() == QLatin1String("boolean")) {
        const QString value = xml.attributes().value(QStringLiteral("value")).toString();
        xml.skipCurrentElement();
        return value == QLatin1String("true");
    } else if (xml.name() == QLatin1String("uint8")) {
        quint8 value = xml.attributes().value(QStringLiteral("value")).toString().toUShort(0, 0);
        xml.skipCurrentElement();
        return value;
    } else if (xml.name() == QLatin1String("uint16")) {
        quint16 value = xml.attributes().value(QStringLiteral("value")).toString().toUShort(0, 0);
        xml.skipCurrentElement();
        return value;
    } else if (xml.name() == QLatin1String("uint32")) {
        quint32 value = xml.attributes().value(QStringLiteral("value")).toString().toUInt(0, 0);
        xml.skipCurrentElement();
        return value;
    } else if (xml.name() == QLatin1String("uint64")) {
        quint64 value = xml.attributes().value(QStringLiteral("value")).toString().toULongLong(0, 0);
        xml.skipCurrentElement();
        return value;
    } else if (xml.name() == QLatin1String("uuid")) {
        QBluetoothUuid uuid;
        const QString value = xml.attributes().value(QStringLiteral("value")).toString();
        if (value.startsWith(QStringLiteral("0x"))) {
            if (value.length() == 6) {
                quint16 v = value.toUShort(0, 0);
                uuid = QBluetoothUuid(v);
            } else if (value.length() == 10) {
                quint32 v = value.toUInt(0, 0);
                uuid = QBluetoothUuid(v);
            }
        } else {
            uuid = QBluetoothUuid(value);
        }
        xml.skipCurrentElement();
        return QVariant::fromValue(uuid);
    } else if (xml.name() == QLatin1String("text")) {
        QString value = xml.attributes().value(QStringLiteral("value")).toString();
        if (xml.attributes().value(QStringLiteral("encoding")) == QLatin1String("hex"))
            value = QString::fromUtf8(QByteArray::fromHex(value.toLatin1()));
        xml.skipCurrentElement();
        return value;
    } else if (xml.name() == QLatin1String("sequence")) {
        QBluetoothServiceInfo::Sequence sequence;

        while (xml.readNextStartElement()) {
            QVariant value = readAttributeValue(xml);
            sequence.append(value);
        }

        return QVariant::fromValue<QBluetoothServiceInfo::Sequence>(sequence);
    } else {
        qCWarning(QT_BT_BLUEZ) << "unknown attribute type"
                               << xml.name().toString()
                               << xml.attributes().value(QStringLiteral("value")).toString();
        xml.skipCurrentElement();
        return QVariant();
    }
}

QT_END_NAMESPACE
