/****************************************************************************
**
** Copyright (C) 2011 Nokia Corporation and/or its subsidiary(-ies).
** All rights reserved.
** Contact: Nokia Corporation (qt-info@nokia.com)
**
** This file is part of the Qt Mobility Components.
**
** $QT_BEGIN_LICENSE:LGPL$
** GNU Lesser General Public License Usage
** This file may be used under the terms of the GNU Lesser General Public
** License version 2.1 as published by the Free Software Foundation and
** appearing in the file LICENSE.LGPL included in the packaging of this
** file. Please review the following information to ensure the GNU Lesser
** General Public License version 2.1 requirements will be met:
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Nokia gives you certain additional
** rights. These rights are described in the Nokia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU General
** Public License version 3.0 as published by the Free Software Foundation
** and appearing in the file LICENSE.GPL included in the packaging of this
** file. Please review the following information to ensure the GNU General
** Public License version 3.0 requirements will be met:
** http://www.gnu.org/copyleft/gpl.html.
**
** Other Usage
** Alternatively, this file may be used in accordance with the terms and
** conditions contained in a signed written agreement between you and Nokia.
**
**
**
**
**
** $QT_END_LICENSE$
**
****************************************************************************/


#include "qbluetoothtransferreply_bluez_p.h"
#include "qbluetoothaddress.h"

#include "bluez/obex_client_p.h"
#include "bluez/obex_manager_p.h"
#include "bluez/obex_agent_p.h"
#include "bluez/obex_transfer_p.h"
#include "qbluetoothtransferreply.h"

#include <QFuture>
#include <QFutureWatcher>
#include <QtConcurrentRun>

static const QLatin1String agentPath("/qt/agent");


QBluetoothTransferReplyBluez::QBluetoothTransferReplyBluez(QIODevice *input, QObject *parent)
:   QBluetoothTransferReply(parent), tempfile(0), source(input),
    m_running(false), m_finished(false), m_size(0),
    m_error(QBluetoothTransferReply::NoError), m_errorStr(), m_transfer_path()
{
    client = new OrgOpenobexClientInterface(QLatin1String("org.openobex.client"), QLatin1String("/"),
                                           QDBusConnection::sessionBus());

//    manager = new OrgOpenobexManagerInterface(QLatin1String("org.openobex"), QLatin1String("/"),
//                                           QDBusConnection::sessionBus());

    qsrand(QTime::currentTime().msec());
    m_agent_path = agentPath;
    m_agent_path.append(QString::fromLatin1("/%1").arg(qrand()));

    agent = new AgentAdaptor(this);

    bool res = QDBusConnection::sessionBus().registerObject(m_agent_path, this);
//    res = QDBusConnection::sessionBus().registerService("org.qt.bt");
    if(!res)
        qDebug() << "Failed Creating dbus objects";

#ifdef NOKIA_BT_SERVICES
    m_obexService = NULL;
    connectToObexServerService();
#endif
}

/*!
    Destroys the QBluetoothTransferReply object.
*/
QBluetoothTransferReplyBluez::~QBluetoothTransferReplyBluez()
{
    QDBusConnection::sessionBus().unregisterObject(m_agent_path);
    delete client;

#ifdef NOKIA_BT_SERVICES
    delete m_obexService;
#endif
}

bool QBluetoothTransferReplyBluez::start()
{
    m_running = true;

//    qDebug() << "Got a:" << source->metaObject()->className();
    QFile *file = qobject_cast<QFile *>(source);

    if(!file){
        tempfile = new QTemporaryFile(this );
        tempfile->open();
//        qDebug() << "Not a QFile, making a copy" << tempfile->fileName();

        QFutureWatcher<bool> *watcher = new QFutureWatcher<bool>();
        QObject::connect(watcher, SIGNAL(finished()), this, SLOT(copyDone()));

        QFuture<bool> results = QtConcurrent::run(QBluetoothTransferReplyBluez::copyToTempFile, tempfile, source);
        watcher->setFuture(results);
    }
    else {
        m_size = file->size();        
        startOPP(file->fileName());
    }
    return true;
}

#ifdef NOKIA_BT_SERVICES
void QBluetoothTransferReplyBluez::connectToObexServerService()
{
    QServiceManager manager;
    QServiceFilter filter("com.nokia.mt.obexserverservice.control");
//    filter.setServiceName("ObexServerServiceControl");

    // find services complying with filter
    QList<QServiceInterfaceDescriptor> foundServices;
    foundServices = manager.findInterfaces(filter);

    if(foundServices.count()) {
        m_obexService = manager.loadInterface(foundServices.at(0));
    }
    if (m_obexService) {
        qDebug() << "connected to service:" << m_obexService;
        connect(m_obexService, SIGNAL(errorUnrecoverableIPCFault(QService::UnrecoverableIPCError)), SLOT(sfwIPCError(QService::UnrecoverableIPCError)));
    } else {
        qDebug() << "failed to connect to Obex server service";
    }
}
#endif

bool QBluetoothTransferReplyBluez::copyToTempFile(QIODevice *to, QIODevice *from)
{
    char *block = new char[4096];
    int size;

    while((size = from->read(block, 4096))) {
        if(size != to->write(block, size)){
            return false;
        }
    }

    delete[] block;
    return true;
}

void QBluetoothTransferReplyBluez::copyDone()
{
    m_size = tempfile->size();
    startOPP(tempfile->fileName());
    QObject::sender()->deleteLater();
}

#ifdef NOKIA_BT_SERVICES
void QBluetoothTransferReplyBluez::sfwIPCError(QService::UnrecoverableIPCError error)
{
    qDebug() << "Connection to Obex server broken:" << error << ". Trying to reconnect...";
    m_obexService->deleteLater();
    QMetaObject::invokeMethod(this, "connectToObexServerService", Qt::QueuedConnection);
}
#endif

void QBluetoothTransferReplyBluez::startOPP(QString filename)
{
    QVariantMap device;
    QStringList files;

    device.insert(QString::fromLatin1("Destination"), address.toString());
    files << filename;

    QDBusObjectPath path(m_agent_path);
    QDBusPendingReply<> sendReply = client->SendFiles(device, files, path);

    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(sendReply, this);
    QObject::connect(watcher, SIGNAL(finished(QDBusPendingCallWatcher*)),
                     this, SLOT(sendReturned(QDBusPendingCallWatcher*)));
}

void QBluetoothTransferReplyBluez::sendReturned(QDBusPendingCallWatcher *watcher)
{

    QDBusPendingReply<> sendReply = *watcher;
    if(sendReply.isError()){
        qDebug() << "Failed to send file"<< sendReply.isError() << sendReply.error().message();
        m_finished = true;
        m_running = false;
        m_errorStr = sendReply.error().message();
        if(m_errorStr == QLatin1String("Could not open file for sending"))
            m_error = QBluetoothTransferReply::FileNotFoundError;
        else if(m_errorStr == QLatin1String("The transfer was canceled"))
            m_error = QBluetoothTransferReply::UserCanceledTransferError;
        else
            m_error = QBluetoothTransferReply::UnknownError;

        // allow time for the developer to connect to the signal
        QMetaObject::invokeMethod(this, "finished", Qt::QueuedConnection, Q_ARG(QBluetoothTransferReply*, this));
    }
}

QBluetoothTransferReply::TransferError QBluetoothTransferReplyBluez::error() const
{
    return m_error;
}

QString QBluetoothTransferReplyBluez::errorString() const
{
    return m_errorStr;
}

void QBluetoothTransferReplyBluez::Complete(const QDBusObjectPath &in0)
{
    Q_UNUSED(in0);
    m_transfer_path.clear();
    m_finished = true;
    m_running = false;

#ifdef NOKIA_BT_SERVICES
    if (m_obexService)
        QMetaObject::invokeMethod(m_obexService, "setTransferFinished", Q_ARG(QString, in0.path()), Q_ARG(bool, true));
#endif
}

void QBluetoothTransferReplyBluez::Error(const QDBusObjectPath &in0, const QString &in1)
{
    Q_UNUSED(in0);
    m_transfer_path.clear();
    m_finished = true;
    m_running = false;
    m_errorStr = in1;
    if (in1 == QLatin1String("Could not open file for sending"))
        m_error = QBluetoothTransferReply::FileNotFoundError;
    else
        m_error = QBluetoothTransferReply::UnknownError;

    emit finished(this);

#ifdef NOKIA_BT_SERVICES
    if (m_obexService)
        QMetaObject::invokeMethod(m_obexService, "setTransferFinished", Q_ARG(QString, in0.path()), Q_ARG(bool, false));
#endif
}

void QBluetoothTransferReplyBluez::Progress(const QDBusObjectPath &in0, qulonglong in1)
{
    Q_UNUSED(in0);
    emit uploadProgress(in1, m_size);

#ifdef NOKIA_BT_SERVICES
    if (m_obexService)
        QMetaObject::invokeMethod(m_obexService, "setTransferProgress", Q_ARG(QString, in0.path()), Q_ARG(quint64, in1), Q_ARG(quint64, m_size));
#endif
}

void QBluetoothTransferReplyBluez::Release()
{
    if(m_errorStr.isEmpty())
        emit finished(this);
}

QString QBluetoothTransferReplyBluez::Request(const QDBusObjectPath &in0)
{
    m_transfer_path = in0.path();

#ifdef NOKIA_BT_SERVICES
    if (m_obexService) {
        QFile *file = qobject_cast<QFile *>(source);
        QMetaObject::invokeMethod(m_obexService, "outgoingFile", Q_ARG(QString, m_transfer_path), Q_ARG(QString, address.toString()), Q_ARG(QString, file->fileName()), Q_ARG(QString, QBluetoothTransferReply::attribute(QBluetoothTransferRequest::TypeAttribute).toString()), Q_ARG(quint64, m_size));
        QMetaObject::invokeMethod(m_obexService, "setTransferStarted", Q_ARG(QString, m_transfer_path));
    }
#endif

    return QString();

}

/*!
    Returns true if this reply has finished; otherwise returns false.
*/
bool QBluetoothTransferReplyBluez::isFinished() const
{
    return m_finished;
}

/*!
    Returns true if this reply is running; otherwise returns false.
*/
bool QBluetoothTransferReplyBluez::isRunning() const
{
    return m_running;
}

void QBluetoothTransferReplyBluez::abort()
{
    if(!m_transfer_path.isEmpty()){
        OrgOpenobexTransferInterface *xfer = new OrgOpenobexTransferInterface(QLatin1String("org.openobex.client"), m_transfer_path,
                                                                              QDBusConnection::sessionBus());
        QDBusPendingReply<> reply = xfer->Cancel();
        reply.waitForFinished();
        if(reply.isError()){
            qDebug() << "Failed to abort transfer" << reply.error();
        }
        delete xfer;

#ifdef NOKIA_BT_SERVICES
        if (m_obexService)
            QMetaObject::invokeMethod(m_obexService, "setTransferFinished", Q_ARG(QString, m_transfer_path), Q_ARG(bool, false));
#endif

    }
}

void QBluetoothTransferReplyBluez::setAddress(const QBluetoothAddress &destination)
{
    address = destination;
}

qint64 QBluetoothTransferReplyBluez::readData(char*, qint64)
{
    return 0;
}

qint64 QBluetoothTransferReplyBluez::writeData(const char*, qint64)
{
    return 0;
}


#include "moc_qbluetoothtransferreply_bluez_p.cpp"
