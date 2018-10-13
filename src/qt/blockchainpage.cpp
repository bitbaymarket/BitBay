#include "blockchainpage.h"
#include "ui_blockchainpage.h"
#include "ui_fractionsdialog.h"

#include "main.h"
#include "base58.h"
#include "txdb.h"
#include "peg.h"
#include "guiutil.h"
#include "blockchainmodel.h"
#include "metatypes.h"
#include "qwt/qwt_plot.h"
#include "qwt/qwt_plot_curve.h"
#include "qwt/qwt_plot_barchart.h"

#include <QPainter>
#include <QClipboard>

#include <string>
#include <vector>

BlockchainPage::BlockchainPage(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::BlockchainPage)
{
    ui->setupUi(this);
    GUIUtil::SetBitBayFonts(this);

    model = new BlockchainModel(this);
    ui->blockchainView->setModel(model);

    connect(model, SIGNAL(rowsAboutToBeInserted(const QModelIndex &,int,int)),
            this, SLOT(updateCurrentBlockIndex()));
    connect(model, SIGNAL(rowsInserted(const QModelIndex &,int,int)),
            this, SLOT(scrollToCurrentBlockIndex()));

    connect(ui->buttonChain, SIGNAL(clicked()), this, SLOT(showChainPage()));
    connect(ui->buttonBlock, SIGNAL(clicked()), this, SLOT(showBlockPage()));
    connect(ui->buttonTx, SIGNAL(clicked()), this, SLOT(showTxPage()));

    connect(ui->blockchainView, SIGNAL(doubleClicked(const QModelIndex &)),
            this, SLOT(openBlock(const QModelIndex &)));
    connect(ui->blockValues, SIGNAL(itemDoubleClicked(QTreeWidgetItem*,int)),
            this, SLOT(openTx(QTreeWidgetItem*,int)));
    connect(ui->txInputs, SIGNAL(itemDoubleClicked(QTreeWidgetItem*,int)),
            this, SLOT(openFractions(QTreeWidgetItem*,int)));
    connect(ui->txOutputs, SIGNAL(itemDoubleClicked(QTreeWidgetItem*,int)),
            this, SLOT(openFractions(QTreeWidgetItem*,int)));

    QFont font = GUIUtil::bitcoinAddressFont();
    qreal pt = font.pointSizeF()*0.8;
    if (pt != .0) {
        font.setPointSizeF(pt);
    } else {
        int px = font.pixelSize()*8/10;
        font.setPixelSize(px);
    }

    QString hstyle = R"(
        QHeaderView::section {
            background-color: rgb(204,203,227);
            color: rgb(64,64,64);
            padding-left: 4px;
            border: 0px solid #6c6c6c;
            border-right: 1px solid #6c6c6c;
            border-bottom: 1px solid #6c6c6c;
            min-height: 16px;
            text-align: left;
        }
    )";
    ui->txValues->setStyleSheet(hstyle);
    ui->txInputs->setStyleSheet(hstyle);
    ui->txOutputs->setStyleSheet(hstyle);
    ui->blockValues->setStyleSheet(hstyle);
    ui->blockchainView->setStyleSheet(hstyle);

    ui->txValues->setFont(font);
    ui->txInputs->setFont(font);
    ui->txOutputs->setFont(font);
    ui->blockValues->setFont(font);
    ui->blockchainView->setFont(font);

    ui->txValues->header()->setFont(font);
    ui->txInputs->header()->setFont(font);
    ui->txOutputs->header()->setFont(font);
    ui->blockValues->header()->setFont(font);
    ui->blockchainView->header()->setFont(font);

    ui->txInputs->header()->resizeSection(0 /*n*/, 50);
    ui->txOutputs->header()->resizeSection(0 /*n*/, 50);
    ui->txInputs->header()->resizeSection(1 /*tx*/, 140);
    ui->txInputs->header()->resizeSection(2 /*addr*/, 280);
    ui->txOutputs->header()->resizeSection(1 /*addr*/, 280);
    ui->txInputs->header()->resizeSection(3 /*value*/, 160);
    ui->txOutputs->header()->resizeSection(2 /*value*/, 160);
    ui->txOutputs->header()->resizeSection(3 /*spent*/, 140);

    auto txInpDelegate = new FractionsItemDelegate(ui->txInputs);
    ui->txInputs->setItemDelegateForColumn(4 /*frac*/, txInpDelegate);

    auto txOutDelegate = new FractionsItemDelegate(ui->txOutputs);
    ui->txOutputs->setItemDelegateForColumn(4 /*frac*/, txOutDelegate);

    connect(ui->lineJumpToBlock, SIGNAL(returnPressed()),
            this, SLOT(jumpToBlock()));
    connect(ui->lineFindBlock, SIGNAL(returnPressed()),
            this, SLOT(openBlockFromInput()));
}

BlockchainPage::~BlockchainPage()
{
    delete ui;
}

BlockchainModel * BlockchainPage::blockchainModel() const
{
    return model;
}

void BlockchainPage::showChainPage()
{
    ui->tabs->setCurrentWidget(ui->pageChain);
}

void BlockchainPage::showBlockPage()
{
    ui->tabs->setCurrentWidget(ui->pageBlock);
}

void BlockchainPage::showTxPage()
{
    ui->tabs->setCurrentWidget(ui->pageTx);
}

void BlockchainPage::jumpToBlock()
{
    bool ok = false;
    int blockNum = ui->lineJumpToBlock->text().toInt(&ok);
    if (!ok) return;

    int n = ui->blockchainView->model()->rowCount();
    int r = n-blockNum;
    if (r<0 || r>=n) return;
    auto mi = ui->blockchainView->model()->index(r, 0);
    ui->blockchainView->setCurrentIndex(mi);
    ui->blockchainView->selectionModel()->select(mi, QItemSelectionModel::Current);
    ui->blockchainView->scrollTo(mi);
}

void BlockchainPage::openBlockFromInput()
{
    bool ok = false;
    int blockNum = ui->lineFindBlock->text().toInt(&ok);
    if (ok) {
        int n = ui->blockchainView->model()->rowCount();
        int r = n-blockNum;
        if (r<0 || r>=n) return;
        auto mi = ui->blockchainView->model()->index(r, 0);
        openBlock(mi);
        return;
    }
    // consider it as hash
    uint256 hash(ui->lineFindBlock->text().toStdString());
    openBlock(hash);
}

void BlockchainPage::updateCurrentBlockIndex()
{
    currentBlockIndex = ui->blockchainView->currentIndex();
}

void BlockchainPage::scrollToCurrentBlockIndex()
{
    ui->blockchainView->scrollTo(currentBlockIndex);
}

void BlockchainPage::openBlock(const QModelIndex & mi)
{
    if (!mi.isValid())
        return;
    openBlock(mi.data(BlockchainModel::HashRole).value<uint256>());
}

void BlockchainPage::openBlock(uint256 hash)
{
    currentBlock = hash;
    QString bhash = QString::fromStdString(currentBlock.ToString());

    LOCK(cs_main);
    if (mapBlockIndex.find(currentBlock) == mapBlockIndex.end())
        return;
    CBlockIndex* pblockindex = mapBlockIndex[currentBlock];
    if (!pblockindex)
        return;
    showBlockPage();
    ui->blockValues->clear();
    ui->blockValues->addTopLevelItem(new QTreeWidgetItem(QStringList({"Height",QString::number(pblockindex->nHeight)})));
    ui->blockValues->addTopLevelItem(new QTreeWidgetItem(QStringList({"Hash",bhash})));

    CBlock block;
    block.ReadFromDisk(pblockindex, true);

    int idx = 0;
    for(const CTransaction & tx : block.vtx) {
        QString stx = "tx"+QString::number(idx);
        QString thash = QString::fromStdString(tx.GetHash().ToString());
        ui->blockValues->addTopLevelItem(new QTreeWidgetItem(QStringList({stx,thash})));
        idx++;
    }
}

static QString scriptToAddress(const CScript& scriptPubKey, bool show_alias =true) {
    int nRequired;
    txnouttype type;
    vector<CTxDestination> addresses;
    if (ExtractDestinations(scriptPubKey, type, addresses, nRequired)) {
        std::string str_addr_all;
        for(const CTxDestination& addr : addresses) {
            std::string str_addr = CBitcoinAddress(addr).ToString();
            if (show_alias) {
                if (str_addr == "bNyZrPLQAMPvYedrVLDcBSd8fbLdNgnRPz") {
                    str_addr = "peginflate";
                }
                else if (str_addr == "bNyZrP2SbrV6v5HqeBoXZXZDE2e4fe6STo") {
                    str_addr = "pegdeflate";
                }
                else if (str_addr == "bNyZrPeFFNP6GFJZCkE82DDN7JC4K5Vrkk") {
                    str_addr = "pegnochange";
                }
            }
            if (!str_addr_all.empty())
                str_addr_all += "\n";
            str_addr_all += str_addr;
        }
        return QString::fromStdString(str_addr_all);
    }
    return QString();
}

static QString displayValue(int64_t nValue) {
    QString sValue = QString::number(nValue);
    if (sValue.length() <8) {
        sValue = sValue.rightJustified(8, QChar(' '));
    }
    sValue.insert(sValue.length()-8, QChar('.'));
    if (sValue.length() > (8+1+3))
        sValue.insert(sValue.length()-8-1-3, QChar(','));
    if (sValue.length() > (8+1+3+1+3))
        sValue.insert(sValue.length()-8-1-3-1-3, QChar(','));
    if (sValue.length() > (8+1+3+1+3+1+3))
        sValue.insert(sValue.length()-8-1-3-1-3-1-3, QChar(','));
    return sValue;
}

void BlockchainPage::openTx(QTreeWidgetItem * item, int column)
{
    Q_UNUSED(column);
    if (!item->text(0).startsWith("tx"))
        return;
    bool tx_idx_ok = false;
    uint tx_idx = item->text(0).mid(2).toUInt(&tx_idx_ok);
    if (!tx_idx_ok)
        return;

    //QString thash = item->text(1);

    LOCK(cs_main);
    if (mapBlockIndex.find(currentBlock) == mapBlockIndex.end())
        return;
    CBlockIndex* pblockindex = mapBlockIndex[currentBlock];
    if (!pblockindex)
        return;

    CBlock block;
    block.ReadFromDisk(pblockindex, true);
    if (tx_idx >= block.vtx.size())
        return;

    CTransaction & tx = block.vtx[tx_idx];
    uint256 hash = tx.GetHash();
    QString thash = QString::fromStdString(hash.ToString());
    QString sheight = QString("%1-%2").arg(pblockindex->nHeight).arg(tx_idx);

    CTxDB txdb("r");
    CPegDB pegdb("r");
    if (!txdb.ContainsTx(hash))
        return;

    showTxPage();
    ui->txValues->clear();
    ui->txValues->addTopLevelItem(new QTreeWidgetItem(QStringList({"Height",sheight})));
    ui->txValues->addTopLevelItem(new QTreeWidgetItem(QStringList({"Hash",thash})));

    MapPrevTx mapInputs;
    MapPrevFractions mapInputsFractions;
    map<uint256, CTxIndex> mapUnused;
    map<uint320, CPegFractions> mapFractionsUnused;
    bool fInvalid = false;
    tx.FetchInputs(txdb, pegdb, mapUnused, mapFractionsUnused, false, false, mapInputs, mapInputsFractions, fInvalid);

    ui->txInputs->clear();
    size_t n_vin = tx.vin.size();
    if (tx.IsCoinBase()) n_vin = 0;
    int64_t nValueIn = 0;
    for (unsigned int i = 0; i < n_vin; i++)
    {
        COutPoint prevout = tx.vin[i].prevout;
        QStringList row;
        row << QString::number(i); // idx, 0

        QString prev_thash = QString::fromStdString(prevout.hash.ToString());
        QString sprev_thash = prev_thash.left(4)+"..."+prev_thash.right(4);
        row << QString("%1:%2").arg(sprev_thash).arg(prevout.n); // tx, 1
        auto prev_input = QString("%1:%2").arg(prev_thash).arg(prevout.n); // tx, 1

        if (mapInputs.find(prevout.hash) != mapInputs.end()) {
            CTransaction& txPrev = mapInputs[prevout.hash].second;
            if (prevout.n < txPrev.vout.size()) {
                auto addr = scriptToAddress(txPrev.vout[prevout.n].scriptPubKey);
                if (addr.isEmpty())
                    row << "N/A"; // address, 2
                else row << addr;

                nValueIn += txPrev.vout[prevout.n].nValue;
                row << displayValue(txPrev.vout[prevout.n].nValue);
            }
            else {
                row << "N/A"; // address, 2
                row << "none"; // value, 3
            }
        }
        else {
            row << "N/A"; // address
            row << "none"; // value
        }

        auto input = new QTreeWidgetItem(row);
        auto fkey = uint320(prevout.hash, prevout.n);
        if (mapInputsFractions.find(fkey) != mapInputsFractions.end()) {
            QVariant vfractions;
            vfractions.setValue(mapInputsFractions[fkey]);
            input->setData(3, Qt::TextAlignmentRole, int(Qt::AlignVCenter | Qt::AlignRight));
            input->setData(4, BlockchainModel::HashRole, prev_input);
            input->setData(4, BlockchainModel::FractionsRole, vfractions);
            input->setData(4, BlockchainModel::PegSupplyRole, pblockindex->nPegSupplyIndex);
        }
        ui->txInputs->addTopLevelItem(input);
    }

    CalculateTransactionFractions(tx, pblockindex,
                                  mapInputs, mapInputsFractions,
                                  mapUnused, mapFractionsUnused);

    CTxIndex txindex;
    txdb.ReadTxIndex(hash, txindex);

    ui->txOutputs->clear();
    size_t n_vout = tx.vout.size();
    if (tx.IsCoinBase()) n_vout = 0;
    int64_t nValueOut = 0;
    for (unsigned int i = 0; i < n_vout; i++)
    {
        QStringList row;
        row << QString::number(i);

        auto addr = scriptToAddress(tx.vout[i].scriptPubKey);
        if (addr.isEmpty())
            row << "N/A"; // address
        else row << addr;

        nValueOut += tx.vout[i].nValue;
        row << displayValue(tx.vout[i].nValue);

        QString titleSpend;
        if (i < txindex.vSpent.size()) {
            CDiskTxPos & txpos = txindex.vSpent[i];
            CTransaction txSpend;
            if (txSpend.ReadFromDisk(txpos)) {
                int vin_idx =0;
                for(const CTxIn &txin : txSpend.vin) {
                    if (txin.prevout.hash == hash && txin.prevout.n == i) {
                        uint256 hashSpend = txSpend.GetHash();
                        QString shashSpend = QString::fromStdString(hashSpend.ToString());
                        QString shashSpendElided = shashSpend.left(4)+"..."+shashSpend.right(4);
                        row << QString("%1:%2").arg(shashSpendElided).arg(vin_idx);
                        titleSpend = QString("%1:%2").arg(shashSpend).arg(vin_idx);
                    }
                    vin_idx++;
                }
            }
        }

        auto output = new QTreeWidgetItem(row);
        auto fkey = uint320(hash, i);
        if (mapFractionsUnused.find(fkey) != mapFractionsUnused.end()) {
            QVariant vFractions;
            vFractions.setValue(mapFractionsUnused[fkey]);
            output->setData(4, BlockchainModel::HashRole, titleSpend);
            output->setData(4, BlockchainModel::FractionsRole, vFractions);
            output->setData(4, BlockchainModel::PegSupplyRole, pblockindex->nPegSupplyIndex);
        }
        output->setData(2, Qt::TextAlignmentRole, int(Qt::AlignVCenter | Qt::AlignRight));
        ui->txOutputs->addTopLevelItem(output);
    }

    if (!tx.IsCoinBase() && !tx.IsCoinStake() && nValueOut < nValueIn) {
        QStringList row;
        row << "Fee";
        row << ""; // address (todo)
        row << displayValue(nValueIn - nValueOut);
        auto output = new QTreeWidgetItem(row);
        output->setData(2, Qt::TextAlignmentRole, int(Qt::AlignVCenter | Qt::AlignRight));
        ui->txOutputs->addTopLevelItem(output);
    }
}

void BlockchainPage::openFractions(QTreeWidgetItem * item, int column)
{
    if (column != 4) // only fractions column
        return;

    auto dlg = new QDialog(this);
    Ui::FractionsDialog ui;
    ui.setupUi(dlg);
    QwtPlot * fplot = new QwtPlot;
    QVBoxLayout *fvbox = new QVBoxLayout;
    fvbox->setMargin(0);
    fvbox->addWidget(fplot);
    ui.chart->setLayout(fvbox);

    QFont font = GUIUtil::bitcoinAddressFont();
    qreal pt = font.pointSizeF()*0.8;
    if (pt != .0) {
        font.setPointSizeF(pt);
    } else {
        int px = font.pixelSize()*8/10;
        font.setPixelSize(px);
    }

    QString hstyle = R"(
        QHeaderView::section {
            background-color: rgb(204,203,227);
            color: rgb(64,64,64);
            padding-left: 4px;
            border: 0px solid #6c6c6c;
            border-right: 1px solid #6c6c6c;
            border-bottom: 1px solid #6c6c6c;
            min-height: 16px;
            text-align: left;
        }
    )";
    ui.fractions->setStyleSheet(hstyle);
    ui.fractions->setFont(font);
    ui.fractions->header()->setFont(font);
    ui.fractions->header()->resizeSection(0 /*n*/, 50);
    ui.fractions->header()->resizeSection(1 /*value*/, 160);

    auto txhash = item->data(4, BlockchainModel::HashRole).toString();
    auto vfractions = item->data(4, BlockchainModel::FractionsRole);
    auto fractions = vfractions.value<CPegFractions>();
    auto fractions_std = fractions.Std();

//    int64_t fdelta[CPegFractions::PEG_SIZE];
//    int64_t fundelta[CPegFractions::PEG_SIZE];
//    fractions_std.ToDeltas(fdelta);
//    CPegFractions fd;
//    fd.FromDeltas(fdelta);

    unsigned long len_test = 0;
    CDataStream fout_test(SER_DISK, CLIENT_VERSION);
    fractions.Pack(fout_test, &len_test);
    ui.packedLabel->setText(tr("Packed: %1 bytes").arg(len_test));

    int64_t f_max = 0;
    for (int i=0; i<CPegFractions::PEG_SIZE; i++) {
        auto f = fractions_std.f[i];
        if (f > f_max) f_max = f;
    }
    //if (f_max == 0)
    //    return; // zero-value fractions

    qreal xs[1200];
    qreal ys[1200];
    QVector<qreal> bs;
    for (int i=0; i<CPegFractions::PEG_SIZE; i++) {
        QStringList row;
        row << QString::number(i) << displayValue(fractions_std.f[i]); // << QString::number(fdelta[i]) << QString::number(fd.f[i]);
        auto row_item = new QTreeWidgetItem(row);
        row_item->setData(0, Qt::TextAlignmentRole, int(Qt::AlignVCenter | Qt::AlignRight));
        row_item->setData(1, Qt::TextAlignmentRole, int(Qt::AlignVCenter | Qt::AlignRight));
        ui.fractions->addTopLevelItem(row_item);
        xs[i] = i;
        ys[i] = qreal(fractions_std.f[i]);
        bs.push_back(qreal(fractions_std.f[i]));
    }

    auto curve = new QwtPlotBarChart;
    //curve->setSamples(xs, ys, 1200);
    curve->setSamples(bs);
    curve->attach(fplot);
    fplot->replot();

    dlg->setWindowTitle(txhash+" "+tr("fractions"));
    dlg->show();
}

// delegate to draw fractions

FractionsItemDelegate::FractionsItemDelegate(QWidget *parent) :
    QItemDelegate(parent)
{
}

FractionsItemDelegate::~FractionsItemDelegate()
{
}

void FractionsItemDelegate::paint(QPainter* p,
                                  const QStyleOptionViewItem& o,
                                  const QModelIndex& index) const
{
    auto vfractions = index.data(BlockchainModel::FractionsRole);
    auto fractions = vfractions.value<CPegFractions>();
    auto fractions_std = fractions.Std();

    int64_t f_max = 0;
    for (int i=0; i<CPegFractions::PEG_SIZE; i++) {
        auto f = fractions_std.f[i];
        if (f > f_max) f_max = f;
    }
    if (f_max == 0)
        return; // zero-value fractions

    auto supply = index.data(BlockchainModel::PegSupplyRole).toInt();

    QPainterPath path;
    QVector<QPointF> points;

    QRect r = o.rect;
    qreal rx = r.x();
    qreal ry = r.y();
    qreal rw = r.width();
    qreal rh = r.height();
    qreal w = CPegFractions::PEG_SIZE;
    qreal h = f_max;

    points.push_back(QPointF(r.x(),r.bottom()));

    for (int i=0; i<CPegFractions::PEG_SIZE; i++) {
        int64_t f = fractions_std.f[i];
        qreal x = rx + qreal(i)*rw/w;
        qreal y = ry + rh - qreal(f)*rh/h;
        points.push_back(QPointF(x,y));
    }

    QPolygonF poly(points);
    path.addPolygon(poly);
    p->setRenderHint( QPainter::Antialiasing );
    p->setBrush( Qt::blue );
    p->setPen( Qt::darkBlue );
    p->drawPath( path );

    p->setPen( Qt::darkGreen );
    qreal pegx = rx + supply*rw/w;
    p->drawLine(QPointF(pegx, ry), QPointF(pegx, ry+rh));
}
