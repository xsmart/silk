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

#include "tablemodel.h"
#include "database.h"

#include <QtCore/QDebug>
#include <QtCore/QMetaObject>
#include <QtCore/QMetaProperty>
#include <QtCore/QStringList>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlError>
#include <QtSql/QSqlRecord>
#include <QtSql/QSqlQuery>

class TableModel::Private : public QObject
{
    Q_OBJECT
public:
    Private(TableModel *parent);
    ~Private();
    void init();

    QString selectSql() const;
private slots:
    void databaseChanged(Database *database);
    void openChanged(bool open);
    void create();
    void select();

private:
    TableModel *q;
    QStringList initialProperties;
    QMap<QString, QString> ifNotExistsMap;
    QMap<QString, QString> autoIncrementMap;
    QMap<QString, QString> primaryKeyMap;
    QStringList fieldNames;

public:
    Database *database;
    QString name;
    QString primaryKey;
    QList<QVariantList> data;
    QHash<int, QByteArray> roleNames;
};

TableModel::Private::Private(TableModel *parent)
    : QObject(parent)
    , q(parent)
    , database(0)
{
    ifNotExistsMap.insert("QSQLITE", " IF NOT EXISTS");
    ifNotExistsMap.insert("QMYSQL", " IF NOT EXISTS");
    ifNotExistsMap.insert("QPSQL", "");
    autoIncrementMap.insert("QSQLITE", " AUTOINCREMENT");
    autoIncrementMap.insert("QMYSQL", " AUTO_INCREMENT");
    autoIncrementMap.insert("QPSQL", "");
    primaryKeyMap.insert("QSQLITE", " PRIMARY KEY");
    primaryKeyMap.insert("QMYSQL", " PRIMARY KEY");
    primaryKeyMap.insert("QPSQL", " PRIMARY KEY");

    connect(q, SIGNAL(databaseChanged(Database*)), this, SLOT(databaseChanged(Database*)));
    const QMetaObject *mo = q->metaObject();
    for (int i = 0; i < mo->propertyCount(); i++) {
        QMetaProperty property = mo->property(i);
//        qDebug() << Q_FUNC_INFO << __LINE__ << property.name() << property.typeName();
        initialProperties.append(property.name());
    }
}

TableModel::Private::~Private()
{
}

void TableModel::Private::init()
{
    if(fieldNames.isEmpty()) {
        const QMetaObject *mo = q->metaObject();
        int j = 0;
        for (int i = initialProperties.count(); i < mo->propertyCount(); i++) {
            QMetaProperty property = mo->property(i);
            roleNames.insert(Qt::UserRole + j, QByteArray(property.name()));
            fieldNames.append(property.name());
            j++;
        }
    }

    if (!database) {
        q->setDatabase(qobject_cast<Database *>(q->QObject::parent()));
    }
}

void TableModel::Private::databaseChanged(Database *database)
{
    disconnect(this, SLOT(openChanged(bool)));
    if (database) {
        connect(database, SIGNAL(openChanged(bool)), this, SLOT(openChanged(bool)));
        openChanged(database->open());
    }
}

void TableModel::Private::openChanged(bool open)
{
    if (open) {
        if (q->name().isEmpty()) {
            qWarning() << "table name is empty.";
            return;
        }
        create();
        select();
    }
}

void TableModel::Private::create()
{
    if (fieldNames.isEmpty()) return;
    QSqlDatabase db = QSqlDatabase::database(database->connectionName());
    if (db.tables().contains(q->name())) return;

    QString type = db.driverName();

    QString sql = QString("CREATE TABLE%2 %1 (").arg(q->name()).arg(ifNotExistsMap.value(type));

    const QMetaObject *mo = q->metaObject();
    int start = initialProperties.count();
    for (int i = start; i < mo->propertyCount(); i++) {
        QMetaProperty property = mo->property(i);
        if (i > start)
            sql.append(QLatin1String(", "));
        sql.append(property.name());

        switch (property.type()) {
        case QVariant::Int:
            sql += QString(" INTEGER");
            break;
        case QVariant::String:
            sql += QString(" TEXT");
            break;
        case QVariant::Bool:
            sql += QString(" bool");
            break;
        case QVariant::Double:
            sql += QString(" DOUBLE");
            break;
        case QVariant::DateTime:
            sql += QString(" TIMESTAMP");
            break;
        default:
            qWarning() << property.typeName() << "is not supported.";
            break;
        }

        if (primaryKey == QString(property.name())) {
            sql += primaryKeyMap.value(type);
            if (property.type() == QVariant::Int) {
                sql += autoIncrementMap.value(type);
            }
        }

        QVariant value = property.read(q);
        if (!value.isNull()) {
            if (property.type() == QVariant::String || property.type() == QVariant::DateTime) {
                sql += QString(" DEFAULT '%1'").arg(value.toString());
            } else {
                sql += QString(" DEFAULT %1").arg(value.toString());
            }
        }
//        qDebug() << Q_FUNC_INFO << __LINE__ << property.name() << property.typeName() << property.read(q) << property.read(q).isNull();
    }

    sql.append(");");
    QSqlQuery query(sql, db);
    if (!query.exec()) {
        qWarning() << query.lastError().text();
    }
}

QString TableModel::Private::selectSql() const
{
    return QString("SELECT %2 FROM %1").arg(q->name()).arg(fieldNames.isEmpty() ? "*" : fieldNames.join(", "));
}

void TableModel::Private::select()
{
    QSqlDatabase db = QSqlDatabase::database(database->connectionName());

    q->beginRemoveRows(QModelIndex(), 0, data.count() - 1);
    data.clear();
    q->endRemoveRows();
    QSqlQuery query(selectSql(), db);
    if (roleNames.isEmpty()) {
        QSqlRecord record = query.record();
        for (int i = 0; i < record.count(); i++) {
            roleNames.insert(i + Qt::UserRole, record.fieldName(i).toUtf8());
        }
    }
    while (query.next()) {
        QVariantList d;
        for (int i = 0; i < roleNames.keys().count(); i++) {
            d.append(query.value(i));
        }
        data.append(d);
    }
    q->beginInsertRows(QModelIndex(), 0, data.count() - 1);
    q->endInsertRows();
}

TableModel::TableModel(QObject *parent)
    : QAbstractListModel(parent)
    , d(new Private(this))
{
}

void TableModel::classBegin()
{

}

void TableModel::componentComplete()
{
    d->init();
}

Database *TableModel::database() const
{
    return d->database;
}

void TableModel::setDatabase(Database *database)
{
    if (d->database == database) return;
    d->database = database;
    emit databaseChanged(database);
}

const QString &TableModel::name() const
{
    return d->name;
}

void TableModel::setName(const QString &name)
{
    if (d->name == name) return;
    d->name = name;
    emit nameChanged(name);
}

const QString &TableModel::primaryKey() const
{
    return d->primaryKey;
}

void TableModel::setPrimaryKey(const QString &primaryKey)
{
    if (d->primaryKey == primaryKey) return;
    d->primaryKey = primaryKey;
    emit primaryKeyChanged(primaryKey);
}

QHash<int, QByteArray> TableModel::roleNames() const
{
    return d->roleNames;
}

int TableModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)
    return d->data.count();
}

QVariant TableModel::data(const QModelIndex &index, int role) const
{
    if (role >= Qt::UserRole) {
        return d->data.at(index.row()).at(role - Qt::UserRole);
    }
    return QVariant();
}

int TableModel::count() const
{
    return rowCount();
}

bool TableModel::insert(const QVariantMap &data)
{
    QStringList keys;
    QStringList placeHolders;
    QVariantList values;
    foreach (const QByteArray &r, d->roleNames.values()) {
        QString field = QString::fromUtf8(r);
        if (data.contains(field)) {
            keys.append(field);
            placeHolders.append("?");
            values.append(data.value(field));
        }
    }

    QSqlDatabase db = QSqlDatabase::database(d->database->connectionName());
    QSqlQuery query(QString("INSERT INTO %1 (%2) VALUES(%3)").arg(name()).arg(keys.join(", ")).arg(placeHolders.join(", ")), db);
    foreach (const QVariant &value, values) {
        query.addBindValue(value);
    }

    bool ret = query.exec();
    if (ret) {
        QString sql = d->selectSql();
        sql += QString(" WHERE %1=%2").arg(d->primaryKey).arg(query.lastInsertId().toInt());
//        qDebug() << Q_FUNC_INFO << __LINE__ << sql << query.lastInsertId();
        QSqlQuery query2(sql, db);
        if (query2.first()) {
            int row = rowCount();
            beginInsertRows(QModelIndex(), row, row);
            QVariantList v;
            for (int i = 0; i < d->roleNames.keys().count(); i++) {
                v.append(query2.value(i));
            }
            d->data.append(v);
            endInsertRows();
            emit countChanged(d->data.count());
        } else {
            qDebug() << Q_FUNC_INFO << __LINE__ << query2.lastError().text() << query2.boundValues();
        }
    } else {
        qWarning() << Q_FUNC_INFO << __LINE__ << query.lastError().text();
    }
    return ret;
}

void TableModel::update(const QVariantMap &data)
{

}

bool TableModel::remove(const QVariantMap &data)
{
    QSqlDatabase db = QSqlDatabase::database(d->database->connectionName());
    QString sql = QString("DELETE FROM %1 WHERE %2=%3;").arg(name()).arg(primaryKey()).arg(data.value(primaryKey()).toInt());
    QSqlQuery query(sql, db);
    bool ret = query.exec();
    if (ret) {
        int count = rowCount();
        int primaryKeyIndex = -1;
        foreach (int role, d->roleNames.keys()) {
            if (d->roleNames.value(role) == d->primaryKey.toUtf8()) {
                primaryKeyIndex = role;
                break;
            }
        }
        if (primaryKeyIndex > -1) {
            for (int i = 0; i < count; i++) {
                if (d->data.at(i).at(primaryKeyIndex - Qt::UserRole) == data.value(primaryKey())) {
                    beginRemoveRows(QModelIndex(), i, i);
                    d->data.removeAt(i);
                    endRemoveRows();
                    emit countChanged(d->data.count());
                    break;
                }
            }
        }
    } else {
        qWarning() << Q_FUNC_INFO << __LINE__ << query.executedQuery() << query.lastError().text();
    }
    return ret;
}

#include "tablemodel.moc"