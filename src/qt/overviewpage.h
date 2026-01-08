// Copyright (c) 2011-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_OVERVIEWPAGE_H
#define BITCOIN_QT_OVERVIEWPAGE_H

#include <interfaces/wallet.h>

#include <consensus/amount.h>

#include <cstdint>
#include <string>

#include <QWidget>
#include <memory>

class ClientModel;
class TransactionFilterProxy;
class TxViewDelegate;
class PlatformStyle;
class WalletModel;

namespace Ui {
    class OverviewPage;
}

QT_BEGIN_NAMESPACE
class QModelIndex;
QT_END_NAMESPACE

/** Overview ("home") page widget */
class OverviewPage : public QWidget
{
    Q_OBJECT

public:
    explicit OverviewPage(const PlatformStyle *platformStyle, QWidget *parent = nullptr);
    ~OverviewPage();

    void setClientModel(ClientModel *clientModel);
    void setWalletModel(WalletModel *walletModel);
    void showOutOfSyncWarning(bool fShow);

public Q_SLOTS:
    void setBalance(const interfaces::WalletBalances& balances);
    void setPrivacy(bool privacy);

Q_SIGNALS:
    void transactionClicked(const QModelIndex &index);
    void outOfSyncWarningClicked();

protected:
    void changeEvent(QEvent* e) override;

private:
    Ui::OverviewPage *ui;
    ClientModel* clientModel{nullptr};
    WalletModel* walletModel{nullptr};
    bool m_privacy{false};

    const PlatformStyle* m_platform_style;

    TxViewDelegate *txdelegate;
    std::unique_ptr<TransactionFilterProxy> filter;

    // --- PoL / Loyalty (GUI) ---
    QString m_pol_miner_id;
    std::string m_pol_tag_hex;

    int m_pol_tip_height{-1};
    bool m_pol_seen{false};
    int m_pol_blocks_seen{0};
    int m_pol_first_seen_height{-1};
    int m_pol_last_seen_height{-1};
    int64_t m_pol_last_seen_time{0};
    int m_pol_points{0};
    int m_pol_level{0};

    bool m_pol_have_amounts{false};
    CAmount m_pol_allowed_subsidy{0};
    CAmount m_pol_base_subsidy{0};

    void clearPolUi(const QString& message);
    void updatePolUiAmounts();

private Q_SLOTS:
    void LimitTransactionRows();
    void updateDisplayUnit();
    void handleTransactionClicked(const QModelIndex &index);
    void updateAlerts(const QString &warnings);

    void refreshPolStatus();
    void polMinerIdChanged(const QString& text);
    void updatePolMinerAddressDropdown();
    void setMonospacedFont(const QFont&);
};

#endif // BITCOIN_QT_OVERVIEWPAGE_H
