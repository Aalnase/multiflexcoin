// Copyright (c) 2011-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/overviewpage.h>
#include <qt/forms/ui_overviewpage.h>

#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/transactionfilterproxy.h>
#include <qt/transactionoverviewwidget.h>
#include <qt/transactiontablemodel.h>

#include <interfaces/node.h>
#include <univalue.h>

#include <qt/walletmodel.h>
#include <qt/addresstablemodel.h>

#include <QAbstractItemDelegate>
#include <QApplication>
#include <QDateTime>
#include <QPixmap>
#include <QtGlobal>
#include <QPainter>
#include <QLineEdit>
#include <QStatusTipEvent>

#include <algorithm>
#include <type_traits>
#include <stdexcept>
#include <map>
#include <string>
#include <vector>

#define DECORATION_SIZE 54
#define NUM_ITEMS 5

Q_DECLARE_METATYPE(interfaces::WalletBalances)

namespace {

// Helper: executeRpc may return either raw result, or an object with {"result": ...}
const UniValue& PolUnwrapRpcResult(const UniValue& maybe_wrapped)
{
    if (!maybe_wrapped.isObject()) return maybe_wrapped;
    const UniValue& res = maybe_wrapped.find_value("result");
    return res.isNull() ? maybe_wrapped : res;
}


template <typename T>
bool PolGetNumField(const UniValue& obj, const char* key, T& out)
{
    if (!obj.isObject()) return false;
    const UniValue& v = obj.find_value(key);
    if (v.isNum()) {
        out = v.getInt<T>();
        return true;
    }
    if (v.isStr()) {
        try {
            const auto s = v.get_str();
            if constexpr (std::is_same_v<T, int64_t>) {
                out = static_cast<T>(std::stoll(s));
                return true;
            } else if constexpr (std::is_integral_v<T>) {
                out = static_cast<T>(std::stoll(s));
                return true;
            }
        } catch (...) {
            return false;
        }
    }
    return false;
}

bool PolGetBoolField(const UniValue& obj, const char* key, bool& out)
{
    if (!obj.isObject()) return false;
    const UniValue& v = obj.find_value(key);
    if (v.isBool()) {
        out = v.get_bool();
        return true;
    }
    if (v.isStr()) {
        const auto s = v.get_str();
        if (s == "true" || s == "1") { out = true; return true; }
        if (s == "false" || s == "0") { out = false; return true; }
    }
    return false;
}


int PolLevelFromPoints(bool seen, int points)
{
    if (!seen || points <= 0) return 0;
    if (points > 24) points = 24;
    return std::min(12, std::max(0, (points + 1) / 2));
}

QString PolLevelIconPath(int level)
{
    const int clamped = std::min(12, std::max(0, level));
    return QString(":/icons/mflex_levels/miner_level_%1.png").arg(clamped, 2, 10, QChar('0'));
}


} // namespace


class TxViewDelegate : public QAbstractItemDelegate
{
    Q_OBJECT
public:
    explicit TxViewDelegate(const PlatformStyle* _platformStyle, QObject* parent = nullptr)
        : QAbstractItemDelegate(parent), platformStyle(_platformStyle)
    {
        connect(this, &TxViewDelegate::width_changed, this, &TxViewDelegate::sizeHintChanged);
    }

    inline void paint(QPainter *painter, const QStyleOptionViewItem &option,
                      const QModelIndex &index ) const override
    {
        painter->save();

        QIcon icon = qvariant_cast<QIcon>(index.data(TransactionTableModel::RawDecorationRole));
        QRect mainRect = option.rect;
        QRect decorationRect(mainRect.topLeft(), QSize(DECORATION_SIZE, DECORATION_SIZE));
        int xspace = DECORATION_SIZE + 8;
        int ypad = 6;
        int halfheight = (mainRect.height() - 2*ypad)/2;
        QRect amountRect(mainRect.left() + xspace, mainRect.top()+ypad, mainRect.width() - xspace, halfheight);
        QRect addressRect(mainRect.left() + xspace, mainRect.top()+ypad+halfheight, mainRect.width() - xspace, halfheight);
        icon = platformStyle->SingleColorIcon(icon);
        icon.paint(painter, decorationRect);

        QDateTime date = index.data(TransactionTableModel::DateRole).toDateTime();
        QString address = index.data(Qt::DisplayRole).toString();
        qint64 amount = index.data(TransactionTableModel::AmountRole).toLongLong();
        bool confirmed = index.data(TransactionTableModel::ConfirmedRole).toBool();
        QVariant value = index.data(Qt::ForegroundRole);
        QColor foreground = option.palette.color(QPalette::Text);
        if(value.canConvert<QBrush>())
        {
            QBrush brush = qvariant_cast<QBrush>(value);
            foreground = brush.color();
        }

        painter->setPen(foreground);
        QRect boundingRect;
        painter->drawText(addressRect, Qt::AlignLeft | Qt::AlignVCenter, address, &boundingRect);

        if(amount < 0)
        {
            foreground = COLOR_NEGATIVE;
        }
        else if(!confirmed)
        {
            foreground = COLOR_UNCONFIRMED;
        }
        else
        {
            foreground = option.palette.color(QPalette::Text);
        }
        painter->setPen(foreground);
        QString amountText = BitcoinUnits::formatWithUnit(unit, amount, true, BitcoinUnits::SeparatorStyle::ALWAYS);
        if(!confirmed)
        {
            amountText = QString("[") + amountText + QString("]");
        }

        QRect amount_bounding_rect;
        painter->drawText(amountRect, Qt::AlignRight | Qt::AlignVCenter, amountText, &amount_bounding_rect);

        painter->setPen(option.palette.color(QPalette::Text));
        QRect date_bounding_rect;
        painter->drawText(amountRect, Qt::AlignLeft | Qt::AlignVCenter, GUIUtil::dateTimeStr(date), &date_bounding_rect);

        // 0.4*date_bounding_rect.width() is used to visually distinguish a date from an amount.
        const int minimum_width = 1.4 * date_bounding_rect.width() + amount_bounding_rect.width();
        const auto search = m_minimum_width.find(index.row());
        if (search == m_minimum_width.end() || search->second != minimum_width) {
            m_minimum_width[index.row()] = minimum_width;
            Q_EMIT width_changed(index);
        }

        painter->restore();
    }

    inline QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const auto search = m_minimum_width.find(index.row());
        const int minimum_text_width = search == m_minimum_width.end() ? 0 : search->second;
        return {DECORATION_SIZE + 8 + minimum_text_width, DECORATION_SIZE};
    }

    BitcoinUnit unit{BitcoinUnit::BTC};

Q_SIGNALS:
    //! An intermediate signal for emitting from the `paint() const` member function.
    void width_changed(const QModelIndex& index) const;

private:
    const PlatformStyle* platformStyle;
    mutable std::map<int, int> m_minimum_width;
};

#include <qt/overviewpage.moc>

OverviewPage::OverviewPage(const PlatformStyle *platformStyle, QWidget *parent) :
    QWidget(parent),
    ui(new Ui::OverviewPage),
    m_platform_style{platformStyle},
    txdelegate(new TxViewDelegate(platformStyle, this))
{
    ui->setupUi(this);

    // PoL / Loyalty moved to dedicated "Loyality" tab.
    if (ui->framePol) ui->framePol->hide();

    // use a SingleColorIcon for the "out of sync warning" icon
    QIcon icon = m_platform_style->SingleColorIcon(QStringLiteral(":/icons/warning"));
    ui->labelTransactionsStatus->setIcon(icon);
    ui->labelWalletStatus->setIcon(icon);

    // Recent transactions
    ui->listTransactions->setItemDelegate(txdelegate);
    ui->listTransactions->setIconSize(QSize(DECORATION_SIZE, DECORATION_SIZE));
    ui->listTransactions->setMinimumHeight(NUM_ITEMS * (DECORATION_SIZE + 2));
    ui->listTransactions->setAttribute(Qt::WA_MacShowFocusRect, false);

    connect(ui->listTransactions, &TransactionOverviewWidget::clicked, this, &OverviewPage::handleTransactionClicked);

    // start with displaying the "out of sync" warnings
    showOutOfSyncWarning(true);
    connect(ui->labelWalletStatus, &QPushButton::clicked, this, &OverviewPage::outOfSyncWarningClicked);
    connect(ui->labelTransactionsStatus, &QPushButton::clicked, this, &OverviewPage::outOfSyncWarningClicked);

    // PoL / Loyalty box
    clearPolUi(tr("Enter miner payout address and press Refresh."));
    connect(ui->buttonPolRefresh, &QPushButton::clicked, this, &OverviewPage::refreshPolStatus);
    if (ui->comboPolMinerAddress->lineEdit()) {
        connect(ui->comboPolMinerAddress->lineEdit(), &QLineEdit::returnPressed, this, &OverviewPage::refreshPolStatus);
        connect(ui->comboPolMinerAddress->lineEdit(), &QLineEdit::textChanged, this, &OverviewPage::polMinerIdChanged);
    }
}

void OverviewPage::handleTransactionClicked(const QModelIndex &index)
{
    if(filter)
        Q_EMIT transactionClicked(filter->mapToSource(index));
}

void OverviewPage::setPrivacy(bool privacy)
{
    m_privacy = privacy;
    clientModel->getOptionsModel()->setOption(OptionsModel::OptionID::MaskValues, privacy);
    const auto& balances = walletModel->getCachedBalance();
    if (balances.balance != -1) {
        setBalance(balances);
    }

    ui->listTransactions->setVisible(!m_privacy);

    const QString status_tip = m_privacy ? tr("Privacy mode activated for the Overview tab. To unmask the values, uncheck Settings->Mask values.") : "";
    setStatusTip(status_tip);
    QStatusTipEvent event(status_tip);
    QApplication::sendEvent(this, &event);

    // Update PoL amounts after toggling privacy mode
    updatePolUiAmounts();
}

OverviewPage::~OverviewPage()
{
    delete ui;
}

void OverviewPage::clearPolUi(const QString& message)
{
    ui->labelPolStatus->setText(message);

    ui->labelPolTag->setText("-");
    ui->labelPolBlocksSeen->setText("-");
ui->labelPolPoints->setText("-");
ui->labelPolLevel->setText("-");
ui->labelPolLevelIcon->clear();

    ui->labelPolAllowed->setText("-");
    ui->labelPolBase->setText("-");
    ui->labelPolBonus->setText("-");
    ui->labelPolLastSeen->setText("-");
    ui->labelPolTipHeight->setText("-");

    m_pol_tip_height = -1;
    m_pol_seen = false;
    m_pol_blocks_seen = 0;
    m_pol_points = 0;
    m_pol_level = 0;
    m_pol_first_seen_height = -1;
    m_pol_last_seen_height = -1;
    m_pol_last_seen_time = 0;

    m_pol_have_amounts = false;
    m_pol_allowed_subsidy = 0;
    m_pol_base_subsidy = 0;
}

void OverviewPage::updatePolUiAmounts()
{
    if (!walletModel || !walletModel->getOptionsModel() || !m_pol_have_amounts) {
        return;
    }

    const auto unit = walletModel->getOptionsModel()->getDisplayUnit();
    const CAmount bonus = std::max<CAmount>(0, m_pol_allowed_subsidy - m_pol_base_subsidy);

    ui->labelPolAllowed->setText(BitcoinUnits::formatWithPrivacy(unit, m_pol_allowed_subsidy, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
    ui->labelPolBase->setText(BitcoinUnits::formatWithPrivacy(unit, m_pol_base_subsidy, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));

    const QString bonus_str = BitcoinUnits::formatWithPrivacy(unit, bonus, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy);

    double pct = 0.0;
    if (m_pol_base_subsidy > 0) {
        pct = 100.0 * (static_cast<double>(bonus) / static_cast<double>(m_pol_base_subsidy));
    }

    ui->labelPolBonus->setText(QString("%1 (%2%)").arg(bonus_str, QString::number(pct, 'f', 2)));
}

void OverviewPage::polMinerIdChanged(const QString& text)
{
    // We no longer derive the tag locally. The mapping (address -> tag12) is
    // consensus-critical and should come from core via getpoladdressstatus.
    m_pol_miner_id = text.trimmed();
    m_pol_tag_hex.clear();

    if (m_pol_miner_id.isEmpty()) {
        clearPolUi(tr("Enter miner address and click Refresh."));
        return;
    }

    clearPolUi(tr("Miner address changed. Click Refresh to update PoL status."));
}


void OverviewPage::refreshPolStatus()
{
    if (!clientModel) {
        clearPolUi(tr("PoL unavailable: no client model"));
        return;
    }

    QString miner_addr = ui->comboPolMinerAddress->currentData().toString();
    if (miner_addr.isEmpty()) {
        miner_addr = ui->comboPolMinerAddress->currentText();
    }
    miner_addr = miner_addr.trimmed();
    if (miner_addr.isEmpty()) {
        clearPolUi(tr("Enter miner address and click Refresh."));
        return;
    }

    // Reset current view (prevents stale values if the RPC call fails)
    m_pol_miner_id = miner_addr;
    m_pol_tag_hex.clear();
    m_pol_tip_height = -1;
    m_pol_seen = false;
    m_pol_blocks_seen = 0;
    m_pol_first_seen_height = -1;
    m_pol_last_seen_height = -1;
    m_pol_last_seen_time = 0;
    m_pol_have_amounts = false;
    m_pol_allowed_subsidy = 0;
    m_pol_base_subsidy = 0;

    try {
        UniValue params(UniValue::VARR);
        params.push_back(miner_addr.toStdString());
        // Optional height: omit to use tip height inside core.

        UniValue rpc_raw = clientModel->node().executeRpc("getpoladdressstatus", params, "");

        const UniValue& st = PolUnwrapRpcResult(rpc_raw);

        // Core echoes back the normalized address (use it if present).
        const UniValue addr_uv = st.find_value("address");
        if (!addr_uv.isNull() && addr_uv.isStr()) {
            m_pol_miner_id = QString::fromStdString(addr_uv.get_str());
        }

        const UniValue tag_uv = st.find_value("miner_tag_hex");
        if (!tag_uv.isNull() && tag_uv.isStr()) {
            m_pol_tag_hex = tag_uv.get_str();
        }

        // Parse fields from RPC result (robust against string/number encodings)
        PolGetNumField<int>(st, "tip_height", m_pol_tip_height);
        PolGetBoolField(st, "seen", m_pol_seen);

        int64_t blocks_seen64 = 0;
        PolGetNumField<int64_t>(st, "blocks_seen", blocks_seen64);
        m_pol_blocks_seen = static_cast<std::remove_reference_t<decltype(m_pol_blocks_seen)>>(blocks_seen64);

        PolGetNumField<int>(st, "first_seen_height", m_pol_first_seen_height);
        PolGetNumField<int>(st, "last_seen_height", m_pol_last_seen_height);
        PolGetNumField<int64_t>(st, "last_seen_time", m_pol_last_seen_time);

        const bool have_allowed = PolGetNumField<int64_t>(st, "allowed_subsidy", m_pol_allowed_subsidy);
        const bool have_base = PolGetNumField<int64_t>(st, "base_subsidy", m_pol_base_subsidy);
        m_pol_have_amounts = have_allowed && have_base;

        // Update UI (text fields)
        ui->labelPolTag->setText(QString::fromStdString(m_pol_tag_hex));
        ui->labelPolBlocksSeen->setText(QString::number(m_pol_blocks_seen));

// Points + Level
m_pol_points = 0;
PolGetNumField<int>(st, "points", m_pol_points);

int level_from_rpc = -1;
if (PolGetNumField<int>(st, "level", level_from_rpc) && level_from_rpc >= 0) {
    m_pol_level = level_from_rpc;
} else {
    m_pol_level = PolLevelFromPoints(m_pol_seen, m_pol_points);
}

ui->labelPolPoints->setText(QString::number(m_pol_points));
ui->labelPolLevel->setText(QString::number(m_pol_level));

const QPixmap pm(PolLevelIconPath(m_pol_level));
if (!pm.isNull()) {
    ui->labelPolLevelIcon->setPixmap(pm);
    ui->labelPolLevelIcon->setToolTip(m_pol_level == 0 ? tr("No level") : tr("Level %1").arg(m_pol_level));
} else {
    ui->labelPolLevelIcon->clear();
}

        ui->labelPolTipHeight->setText(QString::number(m_pol_tip_height));

        QString last_seen_str = "-";
        if (m_pol_seen && m_pol_last_seen_height >= 0) {
            if (m_pol_last_seen_time > 0) {
                const QDateTime dt = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(m_pol_last_seen_time));
                last_seen_str = tr("height %1 @ %2").arg(m_pol_last_seen_height).arg(dt.toString(Qt::ISODate));
            } else {
                last_seen_str = tr("height %1").arg(m_pol_last_seen_height);
            }
        }
        ui->labelPolLastSeen->setText(last_seen_str);

        // Amount fields (allowed/base/bonus)
        updatePolUiAmounts();

        ui->labelPolStatus->setText(m_pol_seen ? tr("PoL status: seen") : tr("PoL status: not seen"));
    }
    catch (const std::exception& e) {
        clearPolUi(tr("PoL RPC error: %1").arg(e.what()));
    }
}



void OverviewPage::updatePolMinerAddressDropdown()
{
    if (!walletModel || !ui || !ui->comboPolMinerAddress) return;

    auto* ab = walletModel->getAddressTableModel();
    if (!ab) return;

    // Preserve current user input/selection
    const QString current_text = ui->comboPolMinerAddress->currentData().toString().isEmpty() ?
        ui->comboPolMinerAddress->currentText().trimmed() :
        ui->comboPolMinerAddress->currentData().toString().trimmed();

    // Collect receive addresses (address book)
    QStringList addrs;
    addrs.reserve(ab->rowCount(QModelIndex()));
    for (int row = 0; row < ab->rowCount(QModelIndex()); row++) {
        const QModelIndex idx0 = ab->index(row, 0, QModelIndex());
        const QString type = ab->data(idx0, AddressTableModel::TypeRole).toString();
        if (type != AddressTableModel::Receive) continue;

        const QString addr = ab->data(ab->index(row, AddressTableModel::Address, QModelIndex()), Qt::EditRole).toString().trimmed();
        if (addr.isEmpty()) continue;
        addrs.push_back(addr);
    }
    addrs.removeDuplicates();
    addrs.sort();

    // Update combo items without triggering refreshes
    ui->comboPolMinerAddress->blockSignals(true);
    if (ui->comboPolMinerAddress->lineEdit()) ui->comboPolMinerAddress->lineEdit()->blockSignals(true);

    ui->comboPolMinerAddress->clear();
    for (const QString& a : addrs) {
        ui->comboPolMinerAddress->addItem(a, a);
    }

    const int found = ui->comboPolMinerAddress->findData(current_text);
    if (found >= 0) ui->comboPolMinerAddress->setCurrentIndex(found);
    else if (!current_text.isEmpty()) ui->comboPolMinerAddress->setEditText(current_text);

    if (ui->comboPolMinerAddress->lineEdit()) ui->comboPolMinerAddress->lineEdit()->blockSignals(false);
    ui->comboPolMinerAddress->blockSignals(false);
}

void OverviewPage::setBalance(const interfaces::WalletBalances& balances)
{
    BitcoinUnit unit = walletModel->getOptionsModel()->getDisplayUnit();
    ui->labelBalance->setText(BitcoinUnits::formatWithPrivacy(unit, balances.balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
    ui->labelUnconfirmed->setText(BitcoinUnits::formatWithPrivacy(unit, balances.unconfirmed_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
    ui->labelImmature->setText(BitcoinUnits::formatWithPrivacy(unit, balances.immature_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
    ui->labelTotal->setText(BitcoinUnits::formatWithPrivacy(unit, balances.balance + balances.unconfirmed_balance + balances.immature_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
    // only show immature (newly mined) balance if it's non-zero, so as not to complicate things
    // for the non-mining users
    bool showImmature = balances.immature_balance != 0;

    ui->labelImmature->setVisible(showImmature);
    ui->labelImmatureText->setVisible(showImmature);
}

void OverviewPage::setClientModel(ClientModel *model)
{
    this->clientModel = model;
    if (model) {
        // Show warning, for example if this is a prerelease version
        connect(model, &ClientModel::alertsChanged, this, &OverviewPage::updateAlerts);
        updateAlerts(model->getStatusBarWarnings());

        connect(model->getOptionsModel(), &OptionsModel::fontForMoneyChanged, this, &OverviewPage::setMonospacedFont);
        setMonospacedFont(clientModel->getOptionsModel()->getFontForMoney());
    }
}

void OverviewPage::setWalletModel(WalletModel *model)
{
    this->walletModel = model;
    if(model && model->getOptionsModel())
    {
        // Set up transaction list
        filter.reset(new TransactionFilterProxy());
        filter->setSourceModel(model->getTransactionTableModel());
        filter->setDynamicSortFilter(true);
        filter->setSortRole(Qt::EditRole);
        filter->setShowInactive(false);
        filter->sort(TransactionTableModel::Date, Qt::DescendingOrder);

        ui->listTransactions->setModel(filter.get());
        ui->listTransactions->setModelColumn(TransactionTableModel::ToAddress);

        connect(filter.get(), &TransactionFilterProxy::rowsInserted, this, &OverviewPage::LimitTransactionRows);
        connect(filter.get(), &TransactionFilterProxy::rowsRemoved, this, &OverviewPage::LimitTransactionRows);
        connect(filter.get(), &TransactionFilterProxy::rowsMoved, this, &OverviewPage::LimitTransactionRows);
        LimitTransactionRows();
        // Keep up to date with wallet
        setBalance(model->getCachedBalance());
        connect(model, &WalletModel::balanceChanged, this, &OverviewPage::setBalance);

        connect(model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &OverviewPage::updateDisplayUnit);

        // Populate PoL miner address dropdown from this wallet's address book
        updatePolMinerAddressDropdown();
        if (auto* ab = model->getAddressTableModel()) {
            connect(ab, &QAbstractItemModel::modelReset, this, &OverviewPage::updatePolMinerAddressDropdown, Qt::UniqueConnection);
            connect(ab, &QAbstractItemModel::rowsInserted, this, &OverviewPage::updatePolMinerAddressDropdown, Qt::UniqueConnection);
            connect(ab, &QAbstractItemModel::rowsRemoved, this, &OverviewPage::updatePolMinerAddressDropdown, Qt::UniqueConnection);
            connect(ab, &QAbstractItemModel::dataChanged, this, &OverviewPage::updatePolMinerAddressDropdown, Qt::UniqueConnection);
        }
    }

    // update the display unit, to not use the default ("BTC")
    updateDisplayUnit();
}

void OverviewPage::changeEvent(QEvent* e)
{
    if (e->type() == QEvent::PaletteChange) {
        QIcon icon = m_platform_style->SingleColorIcon(QStringLiteral(":/icons/warning"));
        ui->labelTransactionsStatus->setIcon(icon);
        ui->labelWalletStatus->setIcon(icon);
    }

    QWidget::changeEvent(e);
}

// Only show most recent NUM_ITEMS rows
void OverviewPage::LimitTransactionRows()
{
    if (filter && ui->listTransactions && ui->listTransactions->model() && filter.get() == ui->listTransactions->model()) {
        for (int i = 0; i < filter->rowCount(); ++i) {
            ui->listTransactions->setRowHidden(i, i >= NUM_ITEMS);
        }
    }
}

void OverviewPage::updateDisplayUnit()
{
    if (walletModel && walletModel->getOptionsModel()) {
        const auto& balances = walletModel->getCachedBalance();
        if (balances.balance != -1) {
            setBalance(balances);
        }

        // Update txdelegate->unit with the current unit
        txdelegate->unit = walletModel->getOptionsModel()->getDisplayUnit();

        ui->listTransactions->update();

        // PoL / Loyalty amounts depend on display unit
        updatePolUiAmounts();
    }
}

void OverviewPage::updateAlerts(const QString &warnings)
{
    this->ui->labelAlerts->setVisible(!warnings.isEmpty());
    this->ui->labelAlerts->setText(warnings);
}

void OverviewPage::showOutOfSyncWarning(bool fShow)
{
    ui->labelWalletStatus->setVisible(fShow);
    ui->labelTransactionsStatus->setVisible(fShow);
}

void OverviewPage::setMonospacedFont(const QFont& f)
{
    ui->labelBalance->setFont(f);
    ui->labelUnconfirmed->setFont(f);
    ui->labelImmature->setFont(f);
    ui->labelTotal->setFont(f);
}
