// Copyright (c) 2009-2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_LOYALTYPAGE_H
#define BITCOIN_QT_LOYALTYPAGE_H

#include <consensus/amount.h>

#include <QWidget>

class ClientModel;
class WalletModel;
class PlatformStyle;

namespace Ui {
class LoyaltyPage;
}

class LoyaltyPage : public QWidget
{
    Q_OBJECT

public:
    explicit LoyaltyPage(const PlatformStyle* platformStyle, QWidget* parent = nullptr);
    ~LoyaltyPage();

    void setClientModel(ClientModel* clientModel);
    void setWalletModel(WalletModel* walletModel);

public Q_SLOTS:
    void refreshPolStatus();

private Q_SLOTS:
    void updateDisplayUnit();
    void polMinerAddressChanged(const QString& txt);
    void updatePolMinerAddressDropdown();

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void clearPolUi(const QString& status_msg = QString());
    void updatePolUiAmounts();
    void updateLevelIcon();

private:
    Ui::LoyaltyPage* ui{nullptr};
    const PlatformStyle* const m_platformStyle;

    ClientModel* clientModel{nullptr};
    WalletModel* walletModel{nullptr};

    QString m_pol_miner_id{};
    bool m_pol_have_amounts{false};

    // Current PoL state
    bool m_pol_seen{false};
    std::string m_pol_tag_hex{};
    int m_pol_tip_height{-1};
    int64_t m_pol_blocks_seen{0};
    int m_pol_first_seen_height{-1};
    int m_pol_last_seen_height{-1};
    int64_t m_pol_last_seen_time{0};
    int m_pol_points{0};
    int m_pol_level{0};
    CAmount m_pol_allowed_subsidy{0};
    CAmount m_pol_base_subsidy{0};
    CAmount m_pol_bonus_subsidy{0};
};

#endif // BITCOIN_QT_LOYALTYPAGE_H
