// Copyright (c) 2018 yshurik
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BLOCKCHAINPAGE_H
#define BLOCKCHAINPAGE_H

#include <QDialog>
#include <QPixmap>
#include <QItemDelegate>
#include <QStyledItemDelegate>
#include "bignum.h"
#include "net.h"

namespace Ui {
    class BlockchainPage;
}

class QTreeView;
class QTreeWidget;
class QModelIndex;
class QTreeWidgetItem;
class BlockchainModel;
class TxDetailsWidget;
class ClientModel;

class BlockchainPage : public QDialog
{
    Q_OBJECT

    enum {
        COL_INP_N = 0,
        COL_INP_TX,
        COL_INP_ADDR,
        COL_INP_VALUE,
        COL_INP_FRACTIONS
    };
    enum {
        COL_OUT_N = 0,
        COL_OUT_TX,
        COL_OUT_ADDR,
        COL_OUT_VALUE,
        COL_OUT_FRACTIONS
    };
    
public:
    explicit BlockchainPage(QWidget *parent = nullptr);
    ~BlockchainPage();

    BlockchainModel * blockchainModel() const;

    void setClientModel(ClientModel *clientModel);
    
public slots:
    void showChainPage();
    void showBlockPage();
    void showTxPage();
    void showAddrPage();
    void showUtxoPage();
    void showNetPage();
    void jumpToTop();
    
private slots:
    void showMempoolPage();
    void openBlock(uint256);
    void openBlock(const QModelIndex &);
    void openBlock(QTreeWidgetItem*,int);
    void openTx(QTreeWidgetItem*,int);
    void openTxFromInput();
    void jumpToBlock();
    void openBlockFromInput();
    void openBalanceAddressFromInput();
    void openUtxoAddressFromInput();
    void updateCurrentBlockIndex();
    void scrollToCurrentBlockIndex();
    void openChainMenu(const QPoint &);
    void openBlockMenu(const QPoint &);
    void updateConnections(const CNodeShortStats &);
    void updateMempool();

private:
    Ui::BlockchainPage* ui;
    BlockchainModel* model;
    TxDetailsWidget* txDetails;
    QPersistentModelIndex currentBlockIndex;
    uint256 currentBlock;
};

class BlockchainPageChainEvents : public QObject
{
    Q_OBJECT
    QTreeView* treeWidget;
public:
    BlockchainPageChainEvents(QTreeView* w, QObject* parent)
        :QObject(parent), treeWidget(w) {}
    ~BlockchainPageChainEvents() override {}
protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
};

class BlockchainPageBlockEvents : public QObject
{
    Q_OBJECT
    QTreeWidget* treeWidget;
public:
    BlockchainPageBlockEvents(QTreeWidget* w, QObject* parent)
        :QObject(parent), treeWidget(w) {}
    ~BlockchainPageBlockEvents() override {}
protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
};

class BlockchainPageTxEvents : public QObject
{
    Q_OBJECT
    QTreeWidget* treeWidget;
public:
    BlockchainPageTxEvents(QTreeWidget* w, QObject* parent)
        :QObject(parent), treeWidget(w) {}
    ~BlockchainPageTxEvents() override {}
protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
};

#endif // BLOCKCHAINPAGE_H
