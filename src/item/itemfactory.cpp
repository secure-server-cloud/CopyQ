/*
    Copyright (c) 2013, Lukas Holecek <hluk@email.cz>

    This file is part of CopyQ.

    CopyQ is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    CopyQ is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with CopyQ.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "itemfactory.h"

#include "common/contenttype.h"
#include "common/client_server.h"
#include "itemwidget.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QModelIndex>
#include <QPluginLoader>

namespace {

const int dummyItemMaxChars = 4096;

bool priorityLessThan(const ItemLoaderInterfacePtr &lhs, const ItemLoaderInterfacePtr &rhs)
{
    return lhs->priority() > rhs->priority();
}

/** Sort plugins by prioritized list of names. */
class PluginSorter {
public:
    PluginSorter(const QStringList &pluginNames) : m_order(pluginNames) {}

    int value(const ItemLoaderInterfacePtr &item) const
    {
        return m_order.indexOf( item->name() );
    }

    bool operator()(const ItemLoaderInterfacePtr &lhs, const ItemLoaderInterfacePtr &rhs) const
    {
        const int l = value(lhs);
        const int r = value(rhs);

        if (l == -1)
            return (r == -1) && lhs->priority() > rhs->priority();

        if (r == -1)
            return true;

        return l < r;
    }

private:
    const QStringList &m_order;
};

class DummyItem : public QLabel, public ItemWidget {
public:
    DummyItem(const QModelIndex &index, QWidget *parent)
        : QLabel(parent)
        , ItemWidget(this)
    {
        setMargin(4);
        setWordWrap(true);
        setTextFormat(Qt::PlainText);
        setText( index.data(contentType::text).toString().left(dummyItemMaxChars) );
        updateSize();
    }

protected:
    virtual void updateSize()
    {
        setMinimumWidth(maximumWidth());
        adjustSize();
    }
};

} // namespace

ItemFactory::ItemFactory(QObject *parent)
    : QObject(parent)
    , m_loaders()
    , m_disabledLoaders()
    , m_loaderChildren()
{ 
    loadPlugins();

    if ( m_loaders.isEmpty() )
        log( QObject::tr("No plugins loaded"), LogNote );
}

ItemFactory::~ItemFactory()
{
}

ItemWidget *ItemFactory::createItem(const ItemLoaderInterfacePtr &loader,
                                    const QModelIndex &index, QWidget *parent)
{
    if ( loader.isNull() || isLoaderEnabled(loader) ) {
        ItemWidget *item = (loader == NULL) ? new DummyItem(index, parent)
                                           : loader->create(index, parent);
        if (item != NULL) {
            item = transformItem(item, index);
            QWidget *w = item->widget();
            QString notes = index.data(contentType::notes).toString();
            if (!notes.isEmpty())
                w->setToolTip(notes);

            m_loaderChildren[w] = loader;
            connect(w, SIGNAL(destroyed(QObject*)), SLOT(loaderChildDestroyed(QObject*)));
            return item;
        }
    }

    return NULL;
}

ItemWidget *ItemFactory::createItem(const QModelIndex &index, QWidget *parent)
{
    foreach (const ItemLoaderInterfacePtr &loader, m_loaders) {
        ItemWidget *item = createItem(loader, index, parent);
        if (item != NULL)
            return item;
    }

    return createItem(ItemLoaderInterfacePtr(), index, parent);
}

ItemWidget *ItemFactory::nextItemLoader(const QModelIndex &index, ItemWidget *current)
{
    return otherItemLoader(index, current, 1);
}

ItemWidget *ItemFactory::previousItemLoader(const QModelIndex &index, ItemWidget *current)
{
    return otherItemLoader(index, current, -1);
}

QStringList ItemFactory::formatsToSave() const
{
    QStringList formats;

    foreach (const ItemLoaderInterfacePtr &loader, m_loaders) {
        if ( isLoaderEnabled(loader) ) {
            foreach ( const QString &format, loader->formatsToSave() ) {
                if ( !formats.contains(format) )
                    formats.append(format);
            }
        }
    }

    if ( !formats.contains("text/plain") )
        formats.prepend("text/plain");

    if ( !formats.contains(mimeItemNotes) )
        formats.append(mimeItemNotes);
    if ( !formats.contains(mimeItems) )
        formats.append(mimeItems);

    return formats;
}

void ItemFactory::setPluginPriority(const QStringList &pluginNames)
{
    qSort( m_loaders.begin(), m_loaders.end(), PluginSorter(pluginNames) );
}

void ItemFactory::setLoaderEnabled(const ItemLoaderInterfacePtr &loader, bool enabled)
{
    if (enabled)
        m_disabledLoaders.remove(loader);
    else
        m_disabledLoaders.insert(loader);
}

bool ItemFactory::isLoaderEnabled(const ItemLoaderInterfacePtr &loader) const
{
    return !m_disabledLoaders.contains(loader);
}

bool ItemFactory::loadItems(const QString &tabName, QAbstractItemModel *model, QFile *file)
{
    foreach (const ItemLoaderInterfacePtr &loader, m_loaders) {
        if ( isLoaderEnabled(loader) && loader->loadItems(tabName, model, file) )
            return true;
    }

    return false;
}

bool ItemFactory::saveItems(const QString &tabName, const QAbstractItemModel &model, QFile *file)
{
    foreach (const ItemLoaderInterfacePtr &loader, m_loaders) {
        if ( isLoaderEnabled(loader) ) {
            file->seek(0);
            if ( loader->saveItems(tabName, model, file) )
                return true;
        }
    }

    return false;
}

void ItemFactory::itemsLoaded(const QString &tabName, QAbstractItemModel *model, QFile *file)
{
    foreach (const ItemLoaderInterfacePtr &loader, m_loaders) {
        if ( isLoaderEnabled(loader) )
            loader->itemsLoaded(tabName, model, file);
    }
}

void ItemFactory::loaderChildDestroyed(QObject *obj)
{
    m_loaderChildren.remove(obj);
}

ItemWidget *ItemFactory::otherItemLoader(const QModelIndex &index, ItemWidget *current, int dir)
{
    Q_ASSERT(dir == -1 || dir == 1);
    Q_ASSERT(current->widget() != NULL);

    QWidget *w = current->widget();
    ItemLoaderInterfacePtr currentLoader = m_loaderChildren[w];
    if ( currentLoader.isNull() )
        return NULL;

    const int currentIndex = m_loaders.indexOf(currentLoader);
    Q_ASSERT(currentIndex != -1);

    const int size = m_loaders.size();
    for (int i = currentIndex + dir; i != currentIndex; i = i + dir) {
        if (i >= size)
            i = i % size;
        else if (i < 0)
            i = size - 1;

        ItemWidget *item = createItem(m_loaders[i], index, w->parentWidget());
        if (item != NULL)
            return item;
    }

    return NULL;
}

bool ItemFactory::loadPlugins()
{
#if defined(COPYQ_WS_X11)
#   ifdef COPYQ_PLUGIN_PREFIX
    QDir pluginsDir(COPYQ_PLUGIN_PREFIX);
#   else
    QDir pluginsDir( QCoreApplication::instance()->applicationDirPath() );
    if ( pluginsDir.dirName() == QString("bin")
         && (!pluginsDir.cdUp() || !pluginsDir.cd("lib") || !pluginsDir.cd("copyq")) )
    {
         return false;
    }
#   endif
#elif defined(Q_OS_MAC)
    QDir pluginsDir( QCoreApplication::instance()->applicationDirPath() );
    if ( pluginsDir.dirName() == "MacOS"
         && (!pluginsDir.cdUp() || !pluginsDir.cdUp() || !pluginsDir.cdUp()) )
    {
        return false;
    }
#else
    QDir pluginsDir( QCoreApplication::instance()->applicationDirPath() );
    if ( !pluginsDir.cd("plugins") )
        return false;
#endif

    if ( !pluginsDir.isReadable() )
        return false;

    foreach (QString fileName, pluginsDir.entryList(QDir::Files)) {
        if ( QLibrary::isLibrary(fileName) ) {
            const QString path = pluginsDir.absoluteFilePath(fileName);
            QPluginLoader pluginLoader(path);
            QObject *plugin = pluginLoader.instance();
            log( QObject::tr("Loading plugin: %1").arg(path), LogNote );
            if (plugin == NULL) {
                log( pluginLoader.errorString(), LogError );
            } else {
                ItemLoaderInterface *loader = qobject_cast<ItemLoaderInterface *>(plugin);
                if (loader == NULL)
                    pluginLoader.unload();
                else
                    m_loaders.append( ItemLoaderInterfacePtr(loader) );
            }
        }
    }

    qSort(m_loaders.begin(), m_loaders.end(), priorityLessThan);

    return true;
}

ItemWidget *ItemFactory::transformItem(ItemWidget *item, const QModelIndex &index)
{
    for ( int i = 0; i < m_loaders.size(); ++i ) {
        const ItemLoaderInterfacePtr &loader = m_loaders[i];
        if ( isLoaderEnabled(loader) ) {
            ItemWidget *newItem = loader->transform(item, index);
            if (newItem != NULL)
                item = newItem;
        }
    }

    return item;
}