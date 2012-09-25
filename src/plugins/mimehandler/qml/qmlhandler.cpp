/* Copyright (c) 2012 Silk Project.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Silk nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL SILK BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "qmlhandler.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QPluginLoader>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkCookie>
#include <QtQml/qqml.h>
#include <QtQml/QQmlEngine>
#include <QtQml/QQmlComponent>

#include <qhttprequest.h>
#include <qhttpreply.h>

#include <silkconfig.h>
#include <silkimportsinterface.h>

#include "httpobject.h"

class QmlHandler::Private : public QObject
{
    Q_OBJECT
public:
    Private(QmlHandler *parent);

    void load(const QFileInfo &fileInfo, QHttpRequest *request, QHttpReply *reply, const QString &message);
private:
    void exec(QQmlComponent *component, QHttpRequest *request, QHttpReply *reply, const QString &message = QString());
    void close(HttpObject *http);

private slots:
    void loadingChanged(bool loading);
    void statusChanged();
    void componentDestroyed(QObject *object);
    void clearQmlCache();

private:
    QmlHandler *q;
    QQmlEngine engine;
    QMap<QObject*, QString> component2root;
    QMap<QObject*, QHttpRequest*> component2request;
    QMap<QObject*, QHttpReply*> component2reply;
    QMap<QObject*, QString> component2message;
    QMap<QObject*, HttpObject*> component2http;
    QMap<QObject*, QHttpRequest*> http2request;
    QMap<QObject*, QHttpReply*> http2reply;
};

QmlHandler::Private::Private(QmlHandler *parent)
    : QObject(parent)
    , q(parent)
{
    qmlRegisterType<SilkAbstractHttpObject>();
    qmlRegisterType<HttpObject>("Silk.HTTP", 1, 1, "Http");

    QDir appDir = QCoreApplication::applicationDirPath();
    QDir importsDir = appDir;
    QString appPath(SILK_APP_PATH);
    // up to system root path
    for (int i = 0; i < appPath.count(QLatin1Char('/')) + 1; i++) {
        importsDir.cdUp();
    }
    importsDir.cd(SILK_IMPORTS_PATH);
    foreach (const QString &lib, importsDir.entryList(QDir::Files)) {
        QPluginLoader pluginLoader(importsDir.absoluteFilePath(lib));
        if (pluginLoader.load()) {
            QObject *object = pluginLoader.instance();
            if (object) {
                SilkImportsInterface *plugin = qobject_cast<SilkImportsInterface *>(object);
                if (plugin) {
                    plugin->silkRegisterObject();
                } else {
                    qWarning() << object;
                }
            } else {
                qWarning() << Q_FUNC_INFO << __LINE__;
            }
        } else {
            qWarning() << pluginLoader.errorString() << importsDir.absoluteFilePath(lib);
        }
    }

    engine.setOfflineStoragePath(appDir.absoluteFilePath(SilkConfig::value("storage.path").toString()));
    engine.addImportPath(":/imports");
    foreach (const QString &importPath, SilkConfig::value("import.path").toStringList()) {
        engine.addImportPath(appDir.absoluteFilePath(importPath));
    }
}

void QmlHandler::Private::load(const QFileInfo &fileInfo, QHttpRequest *request, QHttpReply *reply, const QString &message)
{
    QUrl url;
    if (fileInfo.path().startsWith(':')) {
        url = QUrl(QString("qrc%1").arg(fileInfo.dir().path()));
    } else {
        url = QUrl::fromLocalFile(fileInfo.absoluteDir().path());
    }
    url.setPath(url.path() + "/" + fileInfo.fileName());
    QQmlComponent *component = new QQmlComponent(&engine, url, reply);
    connect(component, SIGNAL(destroyed(QObject *)), this, SLOT(componentDestroyed(QObject *)), Qt::QueuedConnection);
    exec(component, request, reply, message);
}

void QmlHandler::Private::exec(QQmlComponent *component, QHttpRequest *request, QHttpReply *reply, const QString &message)
{
    static bool cache = SilkConfig::value("cache.qml").toBool();
    switch (component->status()) {
    case QQmlComponent::Null:
        // TODO: any check?
        break;
    case QQmlComponent::Error:
        qDebug() << component->errorString();
        emit q->error(500, request, reply, component->errorString());
        break;
    case QQmlComponent::Loading:
        component2request.insert(component, request);
        component2reply.insert(component, reply);
        component2message.insert(component, message);
        connect(component, SIGNAL(statusChanged(QQmlComponent::Status)), this, SLOT(statusChanged()), Qt::UniqueConnection);
        break;
    case QQmlComponent::Ready: {
        HttpObject *http = qobject_cast<HttpObject *>(component->create());
        if (!cache)
            connect(http, SIGNAL(destroyed()), this, SLOT(clearQmlCache()), Qt::QueuedConnection);
        http->method(QString::fromLatin1(request->method()));
        QUrl url(request->url());
        QString query(url.query());
        url.setQuery(QString());
        http->scheme(url.scheme());
        http->host(url.host());
        http->path(url.path());
        http->query(query);
        http->data(QString(request->readAll()));

        QVariantMap requestHeader;
        foreach (const QByteArray &key, request->rawHeaderList()) {
            requestHeader.insert(QString(key), QString(request->rawHeader(key)));
        }
        http->requestHeader(requestHeader);

        QVariantMap cookies;
        foreach (const QNetworkCookie &cookie, request->cookies()) {
            QVariantMap c;
            c.insert(QLatin1String("value"), QString::fromUtf8(cookie.value()));
            c.insert(QLatin1String("expires"), cookie.expirationDate());
            c.insert(QLatin1String("domain"), cookie.domain());
            c.insert(QLatin1String("path"), cookie.path());
            c.insert(QLatin1String("secure"), cookie.isSecure());
            c.insert(QLatin1String("session"), cookie.isSessionCookie());
            cookies.insert(QString::fromUtf8(cookie.name()), c);
        }
        http->requestCookies(cookies);

        if (!message.isEmpty()) http->message(message);
        QMetaObject::invokeMethod(http, "ready");

        component2http.insert(component, http);
        http2request.insert(http, request);
        http2reply.insert(http, reply);
        if (!http->loading()) {
            close(http);
        } else {
            connect(http, SIGNAL(loadingChanged(bool)), this, SLOT(loadingChanged(bool)));
        }
        break; }
    }
}

void QmlHandler::Private::close(HttpObject *http)
{
    if (http2request.contains(http) && http2reply.contains(http)) {
        QHttpRequest *request = http2request.take(http);
        QHttpReply *reply = http2reply.take(http);
        reply->setStatus(http->status());

        QVariantMap header = http->responseHeader();
        foreach (const QString &key, header.keys()) {
            QString value = header.value(key).toString();
            reply->setRawHeader(key.toUtf8(), value.toUtf8());
        }

        QList<QNetworkCookie> cookies;
        foreach (const QString &name, http->responseCookies().keys()) {
            QVariantMap c = http->responseCookies().value(name).toMap();
            QNetworkCookie cookie;
            cookie.setName(name.toUtf8());
            if (c.contains("value")) cookie.setValue(c.value("value").toString().toUtf8());
            if (c.contains("expires")) cookie.setExpirationDate(c.value("expires").toDateTime());
            if (c.contains("domain")) cookie.setDomain(c.value("domain").toString());
            if (c.contains("path")) cookie.setPath(c.value("path").toString());
            if (c.contains("secure")) cookie.setSecure(c.value("secure").toBool());
            cookies.append(cookie);
        }
        reply->setCookies(cookies);

        if (request->method() == "GET" || request->method() == "POST") {
            reply->write(http->out());
        }
        reply->close();
    }
    http->deleteLater();
}

void QmlHandler::Private::loadingChanged(bool loading)
{
    if (!loading) {
        HttpObject *http = qobject_cast<HttpObject *>(sender());
        close(http);
    }
}

void QmlHandler::Private::statusChanged()
{
    QQmlComponent *component = qobject_cast<QQmlComponent *>(sender());
    exec(component, component2request.take(component), component2reply.take(component), component2message.take(component));
}

void QmlHandler::Private::componentDestroyed(QObject *object)
{
    if (component2request.contains(object)) {
        component2request.remove(object);
    }
    if (component2reply.contains(object)) {
        component2reply.remove(object);
    }
    if (component2message.contains(object)) {
        component2message.remove(object);
    }
    if (component2http.contains(object)) {
        HttpObject *http = component2http.take(object);
        http2request.remove(http);
        http2reply.remove(http);
    }
}

void QmlHandler::Private::clearQmlCache()
{
    engine.trimComponentCache();
}

QmlHandler::QmlHandler(QObject *parent)
    : SilkAbstractMimeHandler(parent)
    , d(new Private(this))
{
}

bool QmlHandler::load(const QFileInfo &fileInfo, QHttpRequest *request, QHttpReply *reply, const QString &message)
{
    if (fileInfo.fileName().at(0).isUpper()) {
        return false;
    } else {
        if (!fileInfo.isReadable()){
            emit error(403, request, reply, request->url().toString());
        } else {
            d->load(fileInfo, request, reply, message);
        }
    }
    return true;
}

#include "qmlhandler.moc"