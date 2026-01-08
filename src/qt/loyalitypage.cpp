// Copyright (c) 2009-2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/loyalitypage.h>

#include <qt/addresstablemodel.h>
#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>

#include <interfaces/node.h>

#include <univalue.h>

#include <QComboBox>
#include <QDateTime>
#include <QLineEdit>
#include <QModelIndex>
#include <QPixmap>
#include <QResizeEvent>

#include <algorithm>
#include <set>
#include <vector>

#include <qt/forms/ui_loyalitypage.h>

namespace {

const UniValue& PolUnwrapRpcResult(const UniValue& maybe_wrapped)
{
    // Node::executeRpc returns either a raw result object, OR a wrapper object:
    // { "result": <...> } / { "error": <...> }
    if (!maybe_wrapped.isObject()) return maybe_wrapped;
    const UniValue& res = maybe_wrapped.find_value("result");
    if (!res.isNull()) return res;
    return maybe_wrapped;
}

template <typename T>
bool PolGetNumField(const UniValue& obj, const char* key, T& out)
{
    const UniValue& v = obj.find_value(key);
    if (v.isNull() || !v.isNum()) return false;
    out = v.getInt<T>();
    return true;
}

bool PolGetBoolField(const UniValue& obj, const char* key, bool& out)
{
    const UniValue& v = obj.find_value(key);
    if (v.isNull() || !v.isBool()) return false;
    out = v.get_bool();
    return true;
}

bool PolGetStrField(const UniValue& obj, const char* key, QString& out)
{
    const UniValue& v = obj.find_value(key);
    if (v.isNull() || !v.isStr()) return false;
    out = QString::fromStdString(v.get_str());
    return true;
}

// Level mapping (per your spec):
//   Level 0: no level (seen == false)
//   Level 1: 1-2 points, Level 2: 3-4 points, ..., Level 12: 23-24 points
int PolLevelFromPoints(int points, bool seen)
{
    if (!seen) return 0;
    if (points <= 0) return 0;
    int lvl = (points + 1) / 2;
    if (lvl < 0) lvl = 0;
    if (lvl > 12) lvl = 12;
    return lvl;
}

QString PolLevelIconPath(int level)
{
    int lvl = level;
    if (lvl < 0) lvl = 0;
    if (lvl > 12) lvl = 12;
    return QString(":/icons/mflex_levels/miner_level_%1.png").arg(lvl, 2, 10, QChar('0'));
}

} // namespace

LoyaltyPage::LoyaltyPage(const PlatformStyle* platformStyle, QWidget* parent)
    : QWidget(parent)
    , ui(new Ui::LoyaltyPage)
    , m_platformStyle(platformStyle)
{
    ui->setupUi(this);

    // Make sure the "level icon" row consumes remaining space (so the image can be large).
    if (ui->gridLayoutPol) {
        ui->gridLayoutPol->setRowStretch(10, 1);
        ui->gridLayoutPol->setColumnStretch(1, 1);
        ui->gridLayoutPol->setColumnStretch(2, 1);
    }

    // Wire up UI
    connect(ui->buttonPolRefresh, &QPushButton::clicked, this, &LoyaltyPage::refreshPolStatus);

    if (ui->comboPolMinerAddress) {
        ui->comboPolMinerAddress->setEditable(true);

        if (ui->comboPolMinerAddress->lineEdit()) {
            connect(ui->comboPolMinerAddress->lineEdit(), &QLineEdit::returnPressed, this, &LoyaltyPage::refreshPolStatus);
            connect(ui->comboPolMinerAddress->lineEdit(), &QLineEdit::textChanged, this, &LoyaltyPage::polMinerAddressChanged);
        }
        connect(ui->comboPolMinerAddress, &QComboBox::currentTextChanged, this, &LoyaltyPage::polMinerAddressChanged);
    }

    clearPolUi(tr("PoL status updated."));
}

LoyaltyPage::~LoyaltyPage()
{
    delete ui;
}

void LoyaltyPage::setClientModel(ClientModel* model)
{
    clientModel = model;
}

void LoyaltyPage::setWalletModel(WalletModel* model)
{
    walletModel = model;

    if (!walletModel) return;

    if (walletModel->getOptionsModel()) {
        connect(walletModel->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &LoyaltyPage::updateDisplayUnit);
    }

    // Keep dropdown in sync with address book changes.
    if (auto* ab = walletModel->getAddressTableModel()) {
        connect(ab, &QAbstractItemModel::dataChanged, this, &LoyaltyPage::updatePolMinerAddressDropdown);
        connect(ab, &QAbstractItemModel::rowsInserted, this, &LoyaltyPage::updatePolMinerAddressDropdown);
        connect(ab, &QAbstractItemModel::rowsRemoved, this, &LoyaltyPage::updatePolMinerAddressDropdown);
        connect(ab, &QAbstractItemModel::modelReset, this, &LoyaltyPage::updatePolMinerAddressDropdown);
        connect(ab, &QAbstractItemModel::layoutChanged, this, &LoyaltyPage::updatePolMinerAddressDropdown);
    }

    updateDisplayUnit();
    updatePolMinerAddressDropdown();
}

void LoyaltyPage::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateLevelIcon();
}

void LoyaltyPage::updateDisplayUnit()
{
    updatePolUiAmounts();
    updateLevelIcon();
}

void LoyaltyPage::clearPolUi(const QString& status_msg)
{
    m_pol_seen = false;
    m_pol_tag_hex.clear();
    m_pol_tip_height = -1;
    m_pol_blocks_seen = 0;
    m_pol_first_seen_height = -1;
    m_pol_last_seen_height = -1;
    m_pol_last_seen_time = 0;
    m_pol_points = 0;
    m_pol_level = 0;
    m_pol_allowed_subsidy = 0;
    m_pol_base_subsidy = 0;
    m_pol_bonus_subsidy = 0;
    m_pol_have_amounts = false;

    if (!status_msg.isEmpty()) ui->labelPolStatus->setText(status_msg);
    ui->labelPolTag->setText("-");
    ui->labelPolBlocksSeen->setText("-");
    ui->labelPolTipHeight->setText("-");
    ui->labelPolLastSeen->setText("-");
    ui->labelPolPoints->setText("-");
    ui->labelPolLevel->setText("-");
    ui->labelPolLevelIcon->clear();

    updatePolUiAmounts();
}

void LoyaltyPage::updatePolUiAmounts()
{
    if (!walletModel || !walletModel->getOptionsModel() || !m_pol_have_amounts) {
        ui->labelPolAllowed->setText("-");
        ui->labelPolBase->setText("-");
        ui->labelPolBonus->setText("-");
        return;
    }

    const BitcoinUnit unit = walletModel->getOptionsModel()->getDisplayUnit();

    ui->labelPolAllowed->setText(BitcoinUnits::formatWithUnit(unit, m_pol_allowed_subsidy));
    ui->labelPolBase->setText(BitcoinUnits::formatWithUnit(unit, m_pol_base_subsidy));

    const CAmount bonus = m_pol_allowed_subsidy - m_pol_base_subsidy;
    const double bonus_pct = (m_pol_base_subsidy > 0)
        ? (static_cast<double>(bonus) / static_cast<double>(m_pol_base_subsidy) * 100.0)
        : 0.0;

    ui->labelPolBonus->setText(tr("%1 (%2%)")
        .arg(BitcoinUnits::formatWithUnit(unit, bonus))
        .arg(QString::number(bonus_pct, 'f', 2)));
}

void LoyaltyPage::updateLevelIcon()
{
    if (!ui->labelPolLevelIcon) return;

    const QString path = PolLevelIconPath(m_pol_level);
    QPixmap pm(path);
    if (pm.isNull()) {
        ui->labelPolLevelIcon->clear();
        return;
    }

    // Scale to fit label, preserving aspect ratio (avoid clipping).
    const QSize target = ui->labelPolLevelIcon->size();
    if (target.width() <= 1 || target.height() <= 1) {
        ui->labelPolLevelIcon->setPixmap(pm);
        return;
    }

    pm = pm.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    ui->labelPolLevelIcon->setPixmap(pm);
    ui->labelPolLevelIcon->setToolTip(tr("Level %1").arg(m_pol_level));
}

void LoyaltyPage::polMinerAddressChanged(const QString& txt)
{
    m_pol_miner_id = txt.trimmed();
    clearPolUi();
}

void LoyaltyPage::refreshPolStatus()
{
    if (!clientModel || !walletModel) {
        clearPolUi(tr("PoL status unavailable (no client/wallet)."));
        return;
    }

    QString miner_addr = ui->comboPolMinerAddress->currentData().toString().trimmed();
    if (miner_addr.isEmpty()) miner_addr = ui->comboPolMinerAddress->currentText().trimmed();
    m_pol_miner_id = miner_addr;

    if (miner_addr.isEmpty()) {
        clearPolUi(tr("Enter a Miner Address."));
        return;
    }

    // Build RPC params: getpoladdressstatus <addr> [height]
    // We query at tip (omit height).
    UniValue params(UniValue::VARR);
    params.push_back(miner_addr.toStdString());

    UniValue rpc_raw;
    try {
        rpc_raw = clientModel->node().executeRpc("getpoladdressstatus", params, "");
    } catch (const std::exception& e) {
        clearPolUi(tr("RPC error: %1").arg(e.what()));
        return;
    }

    const UniValue& st = PolUnwrapRpcResult(rpc_raw);

    // Parse fields (defensive: missing fields stay at defaults).
    m_pol_seen = false;
    PolGetBoolField(st, "seen", m_pol_seen);

    QString tag_hex_qstr;
    if (PolGetStrField(st, "miner_tag_hex", tag_hex_qstr)) {
        m_pol_tag_hex = tag_hex_qstr.toStdString();
    } else {
        m_pol_tag_hex.clear();
    }

    m_pol_tip_height = -1;
    PolGetNumField<int>(st, "tip_height", m_pol_tip_height);

    m_pol_blocks_seen = 0;
    PolGetNumField<int64_t>(st, "blocks_seen", m_pol_blocks_seen);

    m_pol_first_seen_height = -1;
    PolGetNumField<int>(st, "first_seen_height", m_pol_first_seen_height);

    m_pol_last_seen_height = -1;
    PolGetNumField<int>(st, "last_seen_height", m_pol_last_seen_height);

    m_pol_last_seen_time = 0;
    PolGetNumField<int64_t>(st, "last_seen_time", m_pol_last_seen_time);

    m_pol_points = 0;
    PolGetNumField<int>(st, "points", m_pol_points);

    // Prefer RPC-provided "level" if present, otherwise compute from points.
    m_pol_level = PolLevelFromPoints(m_pol_points, m_pol_seen);
    PolGetNumField<int>(st, "level", m_pol_level);

    // Subsidies (CAmount is int64_t)
    m_pol_allowed_subsidy = 0;
    PolGetNumField<CAmount>(st, "allowed_subsidy", m_pol_allowed_subsidy);

    m_pol_base_subsidy = 0;
    PolGetNumField<CAmount>(st, "base_subsidy", m_pol_base_subsidy);

    m_pol_bonus_subsidy = 0;
    PolGetNumField<CAmount>(st, "bonus_subsidy", m_pol_bonus_subsidy);

    m_pol_have_amounts = true;

    if (!m_pol_seen) {
        // Keep display consistent with "no level yet"
        m_pol_points = 0;
        m_pol_level = 0;
    }

    // Update UI
    ui->labelPolStatus->setText(tr("PoL status: %1").arg(m_pol_seen ? tr("seen") : tr("not seen")));
    ui->labelPolTag->setText(QString::fromStdString(m_pol_tag_hex));

    ui->labelPolBlocksSeen->setText(QString::number(m_pol_blocks_seen));
    ui->labelPolTipHeight->setText(m_pol_tip_height >= 0 ? QString::number(m_pol_tip_height) : QString("-"));

    if (m_pol_seen && m_pol_last_seen_height >= 0 && m_pol_last_seen_time > 0) {
        const QDateTime dt = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(m_pol_last_seen_time));
        const QString dt_str = dt.toString(Qt::ISODate);
        ui->labelPolLastSeen->setText(tr("height %1 @ %2").arg(m_pol_last_seen_height).arg(dt_str));
    } else {
        ui->labelPolLastSeen->setText(tr("never"));
    }

    ui->labelPolPoints->setText(QString::number(m_pol_points));
    ui->labelPolLevel->setText(QString::number(m_pol_level));

    updatePolUiAmounts();
    updateLevelIcon();
}

void LoyaltyPage::updatePolMinerAddressDropdown()
{
    if (!walletModel || !ui->comboPolMinerAddress) return;

    auto* ab = walletModel->getAddressTableModel();
    if (!ab) return;

    // Preserve current selection/text.
    const QString current_text = ui->comboPolMinerAddress->currentText().trimmed();
    const QString current_addr = ui->comboPolMinerAddress->currentData().toString().trimmed();
    const QString preserve_addr = current_addr.isEmpty() ? current_text : current_addr;

    std::vector<std::pair<QString, QString>> addrs;
    addrs.reserve(static_cast<size_t>(ab->rowCount(QModelIndex())));

    for (int row = 0; row < ab->rowCount(QModelIndex()); row++) {
        const QModelIndex idx_label = ab->index(row, AddressTableModel::Label, QModelIndex());
        const QModelIndex idx_addr = ab->index(row, AddressTableModel::Address, QModelIndex());

        const QString label = ab->data(idx_label, Qt::EditRole).toString().trimmed();
        const QString addr = ab->data(idx_addr, Qt::EditRole).toString().trimmed();
        if (addr.isEmpty()) continue;

        const QString display = label.isEmpty() ? addr : (label + " — " + addr);
        addrs.emplace_back(display, addr);
    }

    std::sort(addrs.begin(), addrs.end());
    addrs.erase(std::unique(addrs.begin(), addrs.end()), addrs.end());

    if (ui->comboPolMinerAddress->lineEdit()) ui->comboPolMinerAddress->lineEdit()->blockSignals(true);
    ui->comboPolMinerAddress->blockSignals(true);

    ui->comboPolMinerAddress->clear();
    for (const auto& it : addrs) {
        ui->comboPolMinerAddress->addItem(it.first, it.second);
    }

    // Restore selection (by address). If not found, restore raw edit text.
    if (!preserve_addr.isEmpty()) {
        int found = -1;
        for (int i = 0; i < ui->comboPolMinerAddress->count(); i++) {
            if (ui->comboPolMinerAddress->itemData(i).toString() == preserve_addr) { found = i; break; }
        }
        if (found >= 0) ui->comboPolMinerAddress->setCurrentIndex(found);
        else ui->comboPolMinerAddress->setEditText(preserve_addr);
    }

    ui->comboPolMinerAddress->blockSignals(false);
    if (ui->comboPolMinerAddress->lineEdit()) ui->comboPolMinerAddress->lineEdit()->blockSignals(false);
}
