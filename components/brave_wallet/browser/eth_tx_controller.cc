/* Copyright (c) 2021 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "brave/components/brave_wallet/browser/eth_tx_controller.h"

#include <utility>

#include "base/bind.h"
#include "base/logging.h"
#include "brave/components/brave_wallet/browser/brave_wallet_service.h"
#include "brave/components/brave_wallet/browser/eth_address.h"
#include "brave/components/brave_wallet/browser/eth_json_rpc_controller.h"
#include "brave/components/brave_wallet/browser/hd_keyring.h"
#include "brave/components/brave_wallet/browser/keyring_controller.h"

namespace brave_wallet {

EthTxController::EthTxController(
    base::WeakPtr<BraveWalletService> wallet_service)
    : wallet_service_(wallet_service),
      tx_state_manager_(std::make_unique<EthTxStateManager>()),
      nonce_tracker_(
          std::make_unique<EthNonceTracker>(tx_state_manager_.get(),
                                            wallet_service_->rpc_controller())),
      pending_tx_tracker_(std::make_unique<EthPendingTxTracker>(
          tx_state_manager_.get(),
          wallet_service_->rpc_controller(),
          nonce_tracker_.get())),
      weak_factory_(this) {}

EthTxController::~EthTxController() = default;

void EthTxController::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void EthTxController::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void EthTxController::AddUnapprovedTransaction(const EthTransaction& tx,
                                               const EthAddress& from) {
  EthTxStateManager::TxMeta meta(tx);
  meta.id = EthTxStateManager::GenerateMetaID();
  // TODO(darkdh): estimate gas price and limit
  meta.tx.set_gas_price(20);
  meta.tx.set_gas_limit(21000);
  meta.from = from;
  meta.status = EthTxStateManager::TransactionStatus::UNAPPROVED;
  tx_state_manager_->AddOrUpdateTx(meta);

  for (Observer& observer : observers_)
    observer.OnNewUnapprovedTx(meta);
}

bool EthTxController::ApproveTransaction(const std::string& tx_meta_id) {
  EthTxStateManager::TxMeta meta;
  if (!tx_state_manager_->GetTx(tx_meta_id, &meta)) {
    LOG(ERROR) << "No transaction found";
    return false;
  }
  if (!meta.last_gas_price) {
    nonce_tracker_->GetNextNonce(
        meta.from, base::BindOnce(&EthTxController::OnGetNextNonce,
                                  weak_factory_.GetWeakPtr(), std::move(meta)));
  } else {
    OnGetNextNonce(EthTxStateManager::TxMeta(meta), true, meta.tx.nonce());
  }

  return true;
}

void EthTxController::OnGetNextNonce(EthTxStateManager::TxMeta meta,
                                     bool success,
                                     uint256_t nonce) {
  if (!success) {
    // TODO(darkdh): Notify observers
    LOG(ERROR) << "GetNextNonce failed";
    return;
  }
  meta.tx.set_nonce(nonce);
  auto* keyring_controller = wallet_service_->keyring_controller();
  DCHECK(!keyring_controller->IsLocked());
  auto* default_keyring = keyring_controller->GetDefaultKeyring();
  DCHECK(default_keyring);
  default_keyring->SignTransaction(meta.from.ToChecksumAddress(), &meta.tx);
  // default_keyring->SignTransaction(default_keyring->GetAddress(0), &meta.tx);
  meta.status = EthTxStateManager::TransactionStatus::APPROVED;
  tx_state_manager_->AddOrUpdateTx(meta);
  PublishTransaction(meta.tx, meta.id);
}

void EthTxController::PublishTransaction(const EthTransaction& tx,
                                         const std::string& tx_meta_id) {
  if (!tx.IsSigned()) {
    LOG(ERROR) << "Transaction must be signed first";
    return;
  }
  EthJsonRpcController* rpc_controller = wallet_service_->rpc_controller();
  DCHECK(rpc_controller);

  rpc_controller->SendRawTransaction(
      tx.GetSignedTransaction(),
      base::BindOnce(&EthTxController::OnPublishTransaction,
                     weak_factory_.GetWeakPtr(), std::string(tx_meta_id)));
}

void EthTxController::OnPublishTransaction(std::string tx_meta_id,
                                           bool status,
                                           const std::string& tx_hash) {
  EthTxStateManager::TxMeta meta;
  if (!tx_state_manager_->GetTx(tx_meta_id, &meta)) {
    DCHECK(false) << "Transaction should be found";
    return;
  }
  if (status) {
    meta.status = EthTxStateManager::TransactionStatus::SUBMITTED;
    meta.tx_hash = tx_hash;
    tx_state_manager_->AddOrUpdateTx(meta);
    pending_tx_tracker_->UpdatePendingTransactions();
  }
}

}  // namespace brave_wallet
