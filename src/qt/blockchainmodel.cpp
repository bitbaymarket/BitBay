
#include "blockchainmodel.h"
#include "guiutil.h"
#include "main.h"
#include "metatypes.h"

#include <QFont>
#include <QDebug>
#include <QApplication>

struct BlockIndexCacheObj {
    uint256 hash;
    int nPegSupplyIndex;
    int nPegVotesInflate;
    int nPegVotesDeflate;
    int nPegVotesNochange;
    unsigned int nFlags;
};

class BlockchainModelPriv {
public:
    int height = 0;
    QStringList columns;
    mutable QCache<int,BlockIndexCacheObj> cache;
    mutable QHash<int,uint256> cachePoints;
};

BlockchainModel::BlockchainModel(QObject *parent) :
    QAbstractItemModel(parent),priv(new BlockchainModelPriv)
{
    priv->columns << tr("Height") << tr("Hash") << tr("Votes") 
                  << tr("Peg") << tr("PegWOK") << tr("PegAOK");
    priv->cache.setMaxCost(100000);
}

BlockchainModel::~BlockchainModel()
{
    delete priv;
}

void BlockchainModel::setNumBlocks(int h) 
{
    beginInsertRows(QModelIndex(), 0, h-priv->height-1);
    priv->height = h;
    endInsertRows();
}

bool BlockchainModel::getItem(int h) const
{
    if (priv->cache.contains(h)) {
        return true;
    }
    
    int h1 = qMax(0, h-200);
    int h2 = qMin(h+200, priv->height);
    int h2_point = (h2/100+1)*100;
    
    LOCK(cs_main);
    uint256 start_hash = hashBestChain;
    if (priv->cachePoints.contains(h2_point)) {
        start_hash = priv->cachePoints[h2_point];
    }
    CBlockIndex* pblockindex = mapBlockIndex[start_hash];
    while (pblockindex->nHeight > h1) {
        if (pblockindex->nHeight % 100 == 0) {
            uint256 bhash = pblockindex->GetBlockHash();
            priv->cachePoints[pblockindex->nHeight] = bhash;
        }
        pblockindex = pblockindex->pprev;
    }
    
    while(pblockindex && pblockindex->nHeight <=h2) {
        uint256 bhash = pblockindex->GetBlockHash();
        auto obj = new BlockIndexCacheObj{bhash, 
                pblockindex->nPegSupplyIndex, 
                pblockindex->nPegVotesInflate, 
                pblockindex->nPegVotesDeflate, 
                pblockindex->nPegVotesNochange,
                pblockindex->nFlags};
        priv->cache.insert(pblockindex->nHeight, obj);
        pblockindex = pblockindex->pnext;
    }
    
    return true;
}

int BlockchainModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    if (parent.isValid())
        return 0;
    return priv->height;
}

int BlockchainModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return priv->columns.length();
}

QVariant BlockchainModel::data(const QModelIndex &index, int role) const
{
    if(!index.isValid())
        return QVariant();

    if(role == Qt::DisplayRole)
    {
        int h = priv->height-index.row();
        getItem(h);
        if (!priv->cache.contains(h)) {
            return QVariant();
        }
        auto obj = priv->cache.object(h);
        
        switch(index.column())
        {
        case Height: {
            return priv->height-index.row();
        }
        case Hash: {
            auto bhash = QString::fromStdString(obj->hash.ToString());
            return bhash.left(4)+"..."+bhash.right(4);
        }
        case Votes: {
            return tr("[%1,%2,%3]").
                    arg(obj->nPegVotesInflate).
                    arg(obj->nPegVotesDeflate).
                    arg(obj->nPegVotesNochange);
        }
        case Peg: {
            return tr("%1").arg(obj->nPegSupplyIndex);
        }
        case PegWOk: {
            if (h < nPegStartHeight)
                return QVariant();
            return tr("%1").arg(obj->nFlags & CBlockIndex::BLOCK_PEG_WFAIL ? "FAIL" : "OK");
        }
        case PegAOk: {
            if (h < nPegStartHeight)
                return QVariant();
            return tr("%1").arg(obj->nFlags & CBlockIndex::BLOCK_PEG_AFAIL ? "FAIL" : "OK");
        }}
    }
    else if (role == Qt::FontRole)
    {
        QFont font = GUIUtil::bitcoinAddressFont();
        qreal pt = font.pointSizeF()*0.7;
        if (pt != .0) {
            font.setPointSizeF(pt);
        } else {
            int px = font.pixelSize()*8/10;
            font.setPixelSize(px);
        }
        return font;
    }
    else if (role == HashRole)
    {
        int h = priv->height-index.row();
        getItem(h);
        if (!priv->cache.contains(h)) {
            return QVariant();
        }
        auto obj = priv->cache.object(h);
        QVariant v;
        v.setValue(obj->hash);
        return v;
    }
    return QVariant();
}

bool BlockchainModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    Q_UNUSED(value);
    Q_UNUSED(role);
    
    if(!index.isValid())
        return false;

    return false;
}

QVariant BlockchainModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if(orientation == Qt::Horizontal)
    {
        if(role == Qt::DisplayRole)
        {
            return priv->columns[section];
        }
    }
    return QVariant();
}

Qt::ItemFlags BlockchainModel::flags(const QModelIndex &index) const
{
    if(!index.isValid())
        return Qt::ItemFlags();
    Qt::ItemFlags retval = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
    return retval;
}

QModelIndex BlockchainModel::index(int row, int column, const QModelIndex &parent) const
{
    if (parent.isValid())
        return QModelIndex();
    return createIndex(row, column, nullptr);
}

QModelIndex BlockchainModel::parent(const QModelIndex &index) const
{
    Q_UNUSED(index);
    return QModelIndex();
}

