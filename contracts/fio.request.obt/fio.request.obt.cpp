/** Fio Request Obt implementation file
 *  Description: The FIO request obt contract supports request for funds and also may record other
 *  block chain transactions (such as send of funds from one FIO address to another).
 *  @author Adam Androulidakis, Casey Gardiner, Ed Rotthoff
 *  @file fio.request.obt.cpp
 *  @license FIO Foundation ( https://github.com/fioprotocol/fio/blob/master/LICENSE ) Dapix
 *
 *  Changes:
 */

#include <eosiolib/asset.hpp>
#include "fio.request.obt.hpp"
#include <fio.address/fio.address.hpp>
#include <fio.fee/fio.fee.hpp>
#include <fio.common/fio.common.hpp>
#include <fio.common/fioerror.hpp>
#include <fio.tpid/fio.tpid.hpp>

namespace fioio {

    class [[eosio::contract("FioRequestObt")]]  FioRequestObt : public eosio::contract {

    private:
        fiotrxts_contexts_table fioTransactionsTable; //Migration Table
        migrledgers_table mgrStatsTable; // Migration Ledger (temp)
        fiorequest_contexts_table fiorequestContextsTable;
        fiorequest_status_table fiorequestStatusTable;
        fionames_table fionames;
        domains_table domains;
        eosio_names_table clientkeys;
        fiofee_table fiofees;
        config appConfig;
        tpids_table tpids;
        recordobt_table recordObtTable;

        eosiosystem::producers_table producers; // Temp reference used for migration

    public:
        explicit FioRequestObt(name s, name code, datastream<const char *> ds)
                : contract(s, code, ds),
                  fioTransactionsTable(_self, _self.value),
                  fiorequestContextsTable(_self, _self.value),
                  fiorequestStatusTable(_self, _self.value),
                  fionames(AddressContract, AddressContract.value),
                  domains(AddressContract, AddressContract.value),
                  fiofees(FeeContract, FeeContract.value),
                  clientkeys(AddressContract, AddressContract.value),
                  tpids(AddressContract, AddressContract.value),
                  producers(SYSTEMACCOUNT, SYSTEMACCOUNT.value), //Temp
                  mgrStatsTable(_self, _self.value), // Temp
                  recordObtTable(_self,_self.value) {
            configs_singleton configsSingleton(FeeContract, FeeContract.value);
            appConfig = configsSingleton.get_or_default(config());
        }

        //TEMP MIGRATION ACTION
        // @abi action
        [[eosio::action]]
        void migrtrx(const uint16_t amount, const string &actor) {
            name executor = name("fio.reqobt");
            name aactor = name(actor);
            require_auth(aactor);

            auto prodbyowner = producers.get_index<"byowner"_n>();
            auto proditer = prodbyowner.find(aactor.value);

            fio_400_assert(proditer != prodbyowner.end(), "actor", actor,
                           "Actor not active producer", ErrorNoFioAddressProducer);

            uint16_t limit = amount;
            uint16_t count = 0;
            bool isSuccessful = false;
            if (amount > 25) { limit = 25; }
            auto migrTable = mgrStatsTable.begin();
            auto TrxTable = fioTransactionsTable.begin();
            //reset index for status migration
            if( migrTable->beginrq != 0 ) {
                mgrStatsTable.modify(migrTable, _self, [&](struct migrledger &strd) {
                    strd.beginrq = 0;
                    strd.currentobt = 0;
                    strd.currentsta = 0;
                });
                return;
            }

            auto trxTable = fioTransactionsTable.find(migrTable->currentobt);
            if (count != limit) {
                while (trxTable != fioTransactionsTable.end()) { //obt record migrate
                    uint64_t id = trxTable->id;
                    if(trxTable->fio_data_type == 4 && trxTable->obt_time == 0) {
                        uint64_t time = trxTable->req_time;
                        string content = trxTable->req_content;
                        if( content == "" ) { content = trxTable->obt_content; }

                        fioTransactionsTable.modify(trxTable, _self, [&](struct fiotrxt_info &strt) {
                            strt.obt_time = time;
                            strt.obt_content = content;
                            strt.req_content = "";
                            strt.req_time = 0;
                        });
                    }
                    count++;

                    if (count == limit) {
                        mgrStatsTable.modify(migrTable, _self, [&](struct migrledger &strc) {
                            strc.currentobt = id + 1;
                        });
                        return;
                    }
                    trxTable++;
                }
            }

            auto statTable = fiorequestStatusTable.find(migrTable->currentsta);
            if (count != limit) { //status table migrate
                while (statTable != fiorequestStatusTable.end()) {
                    uint64_t reqid = statTable->fio_request_id;
                    uint8_t statType = statTable->status;
                    uint64_t timestamp = static_cast<uint64_t>(( statTable->time_stamp / 1000000 )); // remove the 00000 at the end
                    
                    auto trxtByRequestId = fioTransactionsTable.get_index<"byrequestid"_n>();
                    auto fioreqctx_iter = trxtByRequestId.find(reqid);

                    if( fioreqctx_iter != trxtByRequestId.end() ){
                        trxtByRequestId.modify(fioreqctx_iter, _self, [&](struct fiotrxt_info &fr) {
                            fr.fio_data_type = statType;
                            fr.obt_time = timestamp;
                            if (statTable->metadata != "") { fr.obt_content = statTable->metadata; }
                        });
                        count++;
                    }
                    statTable++;
                    if(statTable == fiorequestStatusTable.end()){
                        mgrStatsTable.modify(migrTable, _self, [&](struct migrledger &strc) {
                            strc.currentsta = 0;
                            strc.isFinished = 1;
                        });
                        print(" ALL RECORDS HAVE BEEN COPIED ");
                        return;
                    }
                    if (count == limit) {
                        mgrStatsTable.modify(migrTable, _self, [&](struct migrledger &strc) {
                            strc.currentsta = statTable->id;
                        });
                        return;
                    }
                }
            }
        }
        // END OF TEMP MIGRATION ACTION


         /*******
          * This action will record the send of funds from one FIO address to another, either
          * in response to a request for funds or as a result of a direct send of funds from
          * one user to another
          * @param fio_request_id   This is the one up id of the fio request
          * @param payer_fio_address The payer of the request
          * @param payee_fio_address  The payee (recieve of funds) of the request.
          * @param content  this is the encrypted blob of content containing details of the request.
          * @param max_fee  this is maximum fee the user is willing to pay as a result of this transaction.
          * @param actor  this is the actor (the account which has signed this transaction)
          * @param tpid  this is the tpid for the owner of the domain (this is optional)
          */
        // @abi action
        [[eosio::action]]
        void recordobt(
                const string &fio_request_id,
                const string &payer_fio_address,
                const string &payee_fio_address,
                const string &content,
                const int64_t &max_fee,
                const string &actor,
                const string &tpid) {

            name aactor = name(actor.c_str());
            require_auth(aactor);
            fio_400_assert(validateTPIDFormat(tpid), "tpid", tpid,
                           "TPID must be empty or valid FIO address",
                           ErrorPubKeyValid);
            fio_400_assert(max_fee >= 0, "max_fee", to_string(max_fee), "Invalid fee value",
                           ErrorMaxFeeInvalid);
            fio_400_assert(fio_request_id.length() < 16, "fio_request_id", fio_request_id, "No such FIO Request",
                           ErrorRequestContextNotFound);
            fio_400_assert(payer_fio_address.length() > 0, "payer_fio_address", payer_fio_address,
                           "from fio address not found", ErrorInvalidFioNameFormat);
            fio_400_assert(payee_fio_address.length() > 0, "payee_fio_address", payee_fio_address,
                           "to fio address not found", ErrorInvalidFioNameFormat);

            fio_400_assert(content.size() >= 64 && content.size() <= 432, "content", content,
                           "Requires min 64 max 432 size", ErrorContentLimit);

            FioAddress payerfa;
            getFioAddressStruct(payer_fio_address, payerfa);

            const uint32_t present_time = now();

            uint128_t nameHash = string_to_uint128_hash(payer_fio_address.c_str());
            auto namesbyname = fionames.get_index<"byname"_n>();
            auto fioname_iter = namesbyname.find(nameHash);
            fio_400_assert(fioname_iter != namesbyname.end(), "payer_fio_address", payer_fio_address,
                           "No such FIO Address",
                           ErrorFioNameNotReg);
            uint64_t account = fioname_iter->owner_account;
            uint64_t payernameexp = fioname_iter->expiration;

            fio_400_assert(present_time <= payernameexp, "payer_fio_address", payer_fio_address,
                           "FIO Address expired", ErrorFioNameExpired);

            uint128_t domHash = string_to_uint128_hash(payerfa.fiodomain.c_str());

            auto domainsbyname = domains.get_index<"byname"_n>();
            auto iterdom = domainsbyname.find(domHash);

            fio_400_assert(iterdom != domainsbyname.end(), "payer_fio_address", payer_fio_address,
                           "No such domain",
                           ErrorDomainNotRegistered);
            uint32_t domexp = iterdom->expiration;
            //add 30 days to the domain expiration, this call will work until 30 days past expire.
            domexp = get_time_plus_seconds(domexp,SECONDS30DAYS);

            fio_400_assert(present_time <= domexp, "payer_fio_address", payer_fio_address,
                           "FIO Domain expired", ErrorFioNameExpired);

            auto account_iter = clientkeys.find(account);
            fio_400_assert(account_iter != clientkeys.end(), "payer_fio_address", payer_fio_address,
                           "No such FIO Address",
                           ErrorClientKeyNotFound);
            string payer_key = account_iter->clientkey; // Index 0 is FIO

            nameHash = string_to_uint128_hash(payee_fio_address.c_str());
            namesbyname = fionames.get_index<"byname"_n>();
            auto fioname_iter2 = namesbyname.find(nameHash);

            fio_400_assert(fioname_iter2 != namesbyname.end(), "payee_fio_address", payee_fio_address,
                           "No such FIO Address",
                           ErrorFioNameNotReg);

            fio_403_assert(account == aactor.value, ErrorSignature);

            account = fioname_iter2->owner_account;
            account_iter = clientkeys.find(account);
            fio_400_assert(account_iter != clientkeys.end(), "payee_fio_address", payee_fio_address,
                           "No such FIO Address",
                           ErrorClientKeyNotFound);
            string payee_key = account_iter->clientkey;

            //begin fees, bundle eligible fee logic
            uint128_t endpoint_hash = string_to_uint128_hash(RECORD_OBT_DATA_ENDPOINT);

            auto fees_by_endpoint = fiofees.get_index<"byendpoint"_n>();
            auto fee_iter = fees_by_endpoint.find(endpoint_hash);

            fio_400_assert(fee_iter != fees_by_endpoint.end(), "endpoint_name", RECORD_OBT_DATA_ENDPOINT,
                           "FIO fee not found for endpoint", ErrorNoEndpoint);

            uint64_t fee_type = fee_iter->type;
            fio_400_assert(fee_type == 1, "fee_type", to_string(fee_type),
                           "unexpected fee type for endpoint record_obt_data, expected 1", ErrorNoEndpoint);

            uint64_t fee_amount = 0;

            if (fioname_iter->bundleeligiblecountdown > 1) {
                action{
                        permission_level{_self, "active"_n},
                        AddressContract,
                        "decrcounter"_n,
                        make_tuple(payer_fio_address, 2)
                }.send();
            } else {
                fee_amount = fee_iter->suf_amount;
                fio_400_assert(max_fee >= (int64_t)fee_amount, "max_fee", to_string(max_fee), "Fee exceeds supplied maximum.",
                               ErrorMaxFeeExceeded);

                fio_fees(aactor, asset(fee_amount, FIOSYMBOL), RECORD_OBT_DATA_ENDPOINT);
                process_rewards(tpid, fee_amount, get_self(), aactor);

                if (fee_amount > 0) {
                    INLINE_ACTION_SENDER(eosiosystem::system_contract, updatepower)
                            (SYSTEMACCOUNT, {{_self, "active"_n}},
                             {aactor, true}
                            );
                }
            }
            //end fees, bundle eligible fee logic

            if (fio_request_id.length() > 0) {
                uint64_t currentTime = current_time();
                uint64_t requestId;
                requestId = std::atoi(fio_request_id.c_str());

                auto fioreqctx_iter = fiorequestContextsTable.find(requestId);

                // USED FOR MIGRATION
                auto trxtByRequestId = fioTransactionsTable.get_index<"byrequestid"_n>();
                auto fioreqctx_iter2 = trxtByRequestId.find(requestId);
                // USED FOR MIGRATION

                fio_400_assert(fioreqctx_iter != fiorequestContextsTable.end(), "fio_request_id", fio_request_id,
                               "No such FIO Request", ErrorRequestContextNotFound);

                string payer_account;
                key_to_account(fioreqctx_iter->payer_key, payer_account);
                name payer_acct = name(payer_account.c_str());
                fio_403_assert(aactor == payer_acct, ErrorSignature);

                // USED FOR MIGRATION
                if(fioreqctx_iter2 != trxtByRequestId.end()){
                    trxtByRequestId.modify(fioreqctx_iter2, _self, [&](struct fiotrxt_info &fr) {
                        fr.fio_data_type = static_cast<int64_t>(trxstatus::sent_to_blockchain);
                        fr.obt_content = content;
                        fr.obt_time = present_time;
                    });
                }
                // USED FOR MIGRATION

                //look for other statuses for this request.
                auto statusByRequestId = fiorequestStatusTable.get_index<"byfioreqid"_n>();
                auto fioreqstss_iter = statusByRequestId.find(requestId);
                fio_400_assert(fioreqstss_iter == statusByRequestId.end(), "fio_request_id", fio_request_id,
                               "Only pending requests can be responded.", ErrorRequestStatusInvalid);

                fiorequestStatusTable.emplace(aactor, [&](struct fioreqsts &fr) {
                    fr.id = fiorequestStatusTable.available_primary_key();
                    fr.fio_request_id = requestId;
                    fr.status = static_cast<int64_t >(trxstatus::sent_to_blockchain);
                    fr.metadata = content;
                    fr.time_stamp = currentTime;
                });
            } else {
                const uint64_t id = recordObtTable.available_primary_key();
                const uint128_t toHash = string_to_uint128_hash(payee_fio_address.c_str());
                const uint128_t fromHash = string_to_uint128_hash(payer_fio_address.c_str());
                const string toHashStr = "0x" + to_hex((char *) &toHash, sizeof(toHash));
                const string fromHashStr = "0x" + to_hex((char *) &fromHash, sizeof(fromHash));
                const string payerwtimestr = payer_fio_address + to_string(present_time);
                const string payeewtimestr = payee_fio_address + to_string(present_time);
                const uint128_t payeewtime = string_to_uint128_hash(payeewtimestr.c_str());
                const uint128_t payerwtime = string_to_uint128_hash(payerwtimestr.c_str());

                // USED FOR MIGRATION
                const uint128_t payeeKeyHash = string_to_uint128_hash(payee_key.c_str());
                const uint128_t payerKeyHash = string_to_uint128_hash(payer_key.c_str());

                string payer_account;
                string payee_account;
                key_to_account(payer_key, payer_account);
                key_to_account(payee_key, payee_account);
                name payer_acct = name(payer_account.c_str());
                name payee_acct = name(payee_account.c_str());

                auto trxt_iter = fioTransactionsTable.begin();
                if(trxt_iter != fioTransactionsTable.end()){
                    fioTransactionsTable.emplace(aactor, [&](struct fiotrxt_info &obtinf) {
                        obtinf.id = fioTransactionsTable.available_primary_key();
                        obtinf.payer_fio_addr_hex = fromHash;
                        obtinf.payee_fio_addr_hex = toHash;
                        obtinf.obt_content = content;
                        obtinf.fio_data_type = static_cast<int64_t>(trxstatus::obt_action);
                        obtinf.obt_time = present_time;
                        obtinf.payer_fio_addr = payer_fio_address;
                        obtinf.payee_fio_addr = payee_fio_address;
                        obtinf.payee_key = payee_key;
                        obtinf.payer_key = payer_key;
                        obtinf.payee_account = payee_acct.value;
                        obtinf.payer_account = payer_acct.value;
                    });

                    auto migrTable = mgrStatsTable.begin();
                    if (migrTable != mgrStatsTable.end() && migrTable->beginobt == -1) {
                        mgrStatsTable.modify(migrTable, _self, [&](struct migrledger &strc) {
                            strc.beginobt = id;
                        });
                    }
                }
                // USED FOR MIGRATION

                recordObtTable.emplace(aactor, [&](struct recordobt_info &obtinf) {
                    obtinf.id = id;
                    obtinf.payer_fio_address = fromHash;
                    obtinf.payee_fio_address = toHash;
                    obtinf.payer_fio_address_hex_str = fromHashStr;
                    obtinf.payee_fio_address_hex_str = toHashStr;
                    obtinf.payer_fio_address_with_time = payerwtime;
                    obtinf.payee_fio_address_with_time = payeewtime;
                    obtinf.content = content;
                    obtinf.time_stamp = present_time;
                    obtinf.payer_fio_addr = payer_fio_address;
                    obtinf.payee_fio_addr = payee_fio_address;
                    obtinf.payee_key = payee_key;
                    obtinf.payer_key = payer_key;
                });
            }

            const string response_string = string("{\"status\": \"sent_to_blockchain\",\"fee_collected\":") +
                                     to_string(fee_amount) + string("}");

            if (RECORDOBTRAM > 0) {
                action(
                        permission_level{SYSTEMACCOUNT, "active"_n},
                        "eosio"_n,
                        "incram"_n,
                        std::make_tuple(aactor, RECORDOBTRAM)
                ).send();
            }

            fio_400_assert(transaction_size() <= MAX_TRX_SIZE, "transaction_size", std::to_string(transaction_size()),
              "Transaction is too large", ErrorTransactionTooLarge);

            send_response(response_string.c_str());
        }

       /*********
        * This action will record a request for funds into the FIO protocol.
        * @param payer_fio_address this is the fio address of the payer of the request for funds.
        * @param payee_fio_address this is the requestor of the funds (or the payee) for this request for funds.
        * @param content  this is the blob of encrypted data associated with this request for funds.
        * @param max_fee  this is the maximum fee that the sender of this transaction is willing to pay for this tx.
        * @param actor this is the string representation of the fio account that has signed this transaction
        * @param tpid
        */
        // @abi action
        [[eosio::action]]
        void newfundsreq(
                const string &payer_fio_address,
                const string &payee_fio_address,
                const string &content,
                const int64_t &max_fee,
                const string &actor,
                const string &tpid) {

            const name aActor = name(actor.c_str());
            require_auth(aActor);
            fio_400_assert(validateTPIDFormat(tpid), "tpid", tpid,
                           "TPID must be empty or valid FIO address",
                           ErrorPubKeyValid);
            fio_400_assert(max_fee >= 0, "max_fee", to_string(max_fee), "Invalid fee value",
                           ErrorMaxFeeInvalid);

            fio_400_assert(payer_fio_address.length() > 0, "payer_fio_address", payer_fio_address,
                           "from fio address not specified",
                           ErrorInvalidJsonInput);
            fio_400_assert(payee_fio_address.length() > 0, "payee_fio_address", payee_fio_address,
                           "to fio address not specified",
                           ErrorInvalidJsonInput);

            fio_400_assert(content.size() >= 64 && content.size() <= 296, "content", content,
                           "Requires min 64 max 296 size",
                           ErrorContentLimit);

            const uint32_t present_time = now();

            FioAddress payerfa, payeefa;
            getFioAddressStruct(payer_fio_address, payerfa);
            getFioAddressStruct(payee_fio_address, payeefa);

            uint128_t nameHash = string_to_uint128_hash(payer_fio_address.c_str());
            auto namesbyname = fionames.get_index<"byname"_n>();
            auto fioname_iter2 = namesbyname.find(nameHash);
            fio_400_assert(fioname_iter2 != namesbyname.end(), "payer_fio_address", payer_fio_address,
                           "No such FIO Address",
                           ErrorFioNameNotReg);

            uint64_t account = fioname_iter2->owner_account;
            auto account_iter = clientkeys.find(account);
            fio_400_assert(account_iter != clientkeys.end(), "payer_fio_address", payer_fio_address,
                           "No such FIO Address",
                           ErrorClientKeyNotFound);
            string payer_key = account_iter->clientkey; // Index 0 is FIO

            nameHash = string_to_uint128_hash(payee_fio_address.c_str());
            namesbyname = fionames.get_index<"byname"_n>();
            auto fioname_iter = namesbyname.find(nameHash);
            fio_400_assert(fioname_iter != namesbyname.end(), "payee_fio_address", payee_fio_address,
                           "No such FIO Address",
                           ErrorFioNameNotReg);

            account = fioname_iter->owner_account;
            account_iter = clientkeys.find(account);
            fio_400_assert(account_iter != clientkeys.end(), "payee_fio_address", payee_fio_address,
                           "No such FIO Address",
                           ErrorClientKeyNotFound);
            string payee_key = account_iter->clientkey;

            const uint64_t payeenameexp = fioname_iter->expiration;
            fio_400_assert(present_time <= payeenameexp, "payee_fio_address", payee_fio_address,
                           "FIO Address expired", ErrorFioNameExpired);

            const uint128_t domHash = string_to_uint128_hash(payeefa.fiodomain.c_str());
            auto domainsbyname = domains.get_index<"byname"_n>();
            auto iterdom = domainsbyname.find(domHash);

            fio_400_assert(iterdom != domainsbyname.end(), "payee_fio_address", payee_fio_address,
                           "No such domain",
                           ErrorDomainNotRegistered);

            //add 30 days to the domain expiration, this call will work until 30 days past expire.
            const uint64_t domexp = get_time_plus_seconds(iterdom->expiration,SECONDS30DAYS);

            fio_400_assert(present_time <= domexp, "payee_fio_address", payee_fio_address,
                           "FIO Domain expired", ErrorFioNameExpired);

            fio_403_assert(account == aActor.value, ErrorSignature);

            //begin fees, bundle eligible fee logic
            const uint128_t endpoint_hash = string_to_uint128_hash(NEW_FUNDS_REQUEST_ENDPOINT);
            auto fees_by_endpoint = fiofees.get_index<"byendpoint"_n>();
            auto fee_iter = fees_by_endpoint.find(endpoint_hash);

            fio_400_assert(fee_iter != fees_by_endpoint.end(), "endpoint_name", NEW_FUNDS_REQUEST_ENDPOINT,
                           "FIO fee not found for endpoint", ErrorNoEndpoint);

            const uint64_t fee_type = fee_iter->type;

            fio_400_assert(fee_type == 1, "fee_type", to_string(fee_type),
                           "unexpected fee type for endpoint new_funds_request, expected 1",
                           ErrorNoEndpoint);

            uint64_t fee_amount = 0;

            if (fioname_iter->bundleeligiblecountdown > 1) {
                action{
                        permission_level{_self, "active"_n},
                        AddressContract,
                        "decrcounter"_n,
                        make_tuple(payee_fio_address, 2)
                }.send();
            } else {
                fee_amount = fee_iter->suf_amount;
                fio_400_assert(max_fee >= (int64_t)fee_amount, "max_fee", to_string(max_fee), "Fee exceeds supplied maximum.",
                               ErrorMaxFeeExceeded);

                fio_fees(aActor, asset(fee_amount, FIOSYMBOL), NEW_FUNDS_REQUEST_ENDPOINT);
                process_rewards(tpid, fee_amount, get_self(),aActor);

                if (fee_amount > 0) {
                    INLINE_ACTION_SENDER(eosiosystem::system_contract, updatepower)
                            (SYSTEMACCOUNT, {{_self, "active"_n}},
                             {aActor, true}
                            );
                }
            }
            //end fees, bundle eligible fee logic

            const uint64_t id = fiorequestContextsTable.available_primary_key();
            const uint64_t currentTime = now();
            const uint128_t toHash = string_to_uint128_hash(payee_fio_address.c_str());
            const uint128_t fromHash = string_to_uint128_hash(payer_fio_address.c_str());
            const string payerwtimestr = payer_fio_address + to_string(currentTime);
            const string payeewtimestr = payee_fio_address + to_string(currentTime);
            const uint128_t payeewtime = string_to_uint128_hash(payeewtimestr.c_str());
            const uint128_t payerwtime = string_to_uint128_hash(payerwtimestr.c_str());
            const string toHashStr = "0x" + to_hex((char *) &toHash, sizeof(toHash));
            const string fromHashStr = "0x" + to_hex((char *) &fromHash, sizeof(fromHash));

            // USED FOR MIGRATION
            const uint128_t payeeKeyHash = string_to_uint128_hash(payee_key.c_str());
            const uint128_t payerKeyHash = string_to_uint128_hash(payer_key.c_str());
            // USED FOR MIGRATION

            fiorequestContextsTable.emplace(aActor, [&](struct fioreqctxt &frc) {
                frc.fio_request_id = id;
                frc.payer_fio_address = fromHash;
                frc.payee_fio_address = toHash;
                frc.payer_fio_address_hex_str = fromHashStr;
                frc.payee_fio_address_hex_str = toHashStr;
                frc.payer_fio_address_with_time= payerwtime;
                frc.payee_fio_address_with_time=payeewtime;
                frc.content = content;
                frc.time_stamp = currentTime;
                frc.payer_fio_addr = payer_fio_address;
                frc.payee_fio_addr = payee_fio_address;
                frc.payee_key = payee_key;
                frc.payer_key = payer_key;
            });

            // USED FOR MIGRATION
            auto trxt_iter = fioTransactionsTable.begin();
            if(trxt_iter != fioTransactionsTable.end()){
                string payer_account;
                string payee_account;
                key_to_account(payer_key, payer_account);
                key_to_account(payee_key, payee_account);
                name payer_acct = name(payer_account.c_str());
                name payee_acct = name(payee_account.c_str());

                fioTransactionsTable.emplace(aActor, [&](struct fiotrxt_info &frc) {
                    frc.id = fioTransactionsTable.available_primary_key();
                    frc.fio_request_id = id;
                    frc.payer_fio_addr_hex = fromHash;
                    frc.payee_fio_addr_hex = toHash;
                    frc.req_content = content;
                    frc.fio_data_type = static_cast<int64_t>(trxstatus::requested);
                    frc.req_time = currentTime;
                    frc.payer_fio_addr = payer_fio_address;
                    frc.payee_fio_addr = payee_fio_address;
                    frc.payee_key = payee_key;
                    frc.payer_key = payer_key;
                    frc.payee_account = payee_acct.value;
                    frc.payer_account = payer_acct.value;
                });

                auto migrTable = mgrStatsTable.begin();
                if (migrTable != mgrStatsTable.end() && migrTable->beginrq == -1) {
                    mgrStatsTable.modify(migrTable, _self, [&](struct migrledger &strc) {
                        strc.beginrq = id;
                    });
                }
            }
            // USED FOR MIGRATION

           const string response_string = string("{\"fio_request_id\":") + to_string(id) + string(",\"status\":\"requested\"") +
                                    string(",\"fee_collected\":") + to_string(fee_amount) + string("}");


            if (NEWFUNDSREQUESTRAM > 0) {
                action(
                        permission_level{SYSTEMACCOUNT, "active"_n},
                        "eosio"_n,
                        "incram"_n,
                        std::make_tuple(aActor, NEWFUNDSREQUESTRAM)
                ).send();
            }

            fio_400_assert(transaction_size() <= MAX_TRX_SIZE, "transaction_size", std::to_string(transaction_size()),
              "Transaction is too large", ErrorTransactionTooLarge);

           send_response(response_string.c_str());
        }

         /********
          * this action will add a rejection status to the request for funds with the specified request id.
          * the input fiorequest id will be verified to ensure there is a request in the contexts table matching this id
          * before the status record is added to the index tables.
          * @param fio_request_id this is the id of the request in the fio request contexts table.
          * @param max_fee  this is the maximum fee that the sender of this transaction is willing to pay.
          * @param actor  this is the string representation of the FIO account that is associated with the signer of this tx.
          * @param tpid  this is the fio address of the domain owner associated with this request.
          */
        // @abi action
        [[eosio::action]]
        void rejectfndreq(
                const string &fio_request_id,
                const int64_t &max_fee,
                const string &actor,
                const string &tpid) {

            const name aactor = name(actor.c_str());
            require_auth(aactor);
            fio_400_assert(validateTPIDFormat(tpid), "tpid", tpid,
                           "TPID must be empty or valid FIO address",
                           ErrorPubKeyValid);
            fio_400_assert(max_fee >= 0, "max_fee", to_string(max_fee), "Invalid fee value",
                           ErrorMaxFeeInvalid);

            fio_400_assert(fio_request_id.length() > 0 && fio_request_id.length() < 16, "fio_request_id", fio_request_id, "No value specified",
                           ErrorRequestContextNotFound);

           const uint64_t currentTime = current_time();
            uint64_t requestId;

            requestId = std::atoi(fio_request_id.c_str());

            auto fioreqctx_iter = fiorequestContextsTable.find(requestId);

            // USED FOR MIGRATION
            auto trxtByRequestId = fioTransactionsTable.get_index<"byrequestid"_n>();
            auto fioreqctx2_iter = trxtByRequestId.find(requestId);
            // USED FOR MIGRATION

            fio_400_assert(fioreqctx_iter != fiorequestContextsTable.end(), "fio_request_id", fio_request_id,
                           "No such FIO Request", ErrorRequestContextNotFound);

            //verify pending request only
            auto statusByRequestId = fiorequestStatusTable.get_index<"byfioreqid"_n>();
            auto fioreqstss_iter = statusByRequestId.find(requestId);
            fio_400_assert(fioreqstss_iter->status == 0, "fio_request_id", fio_request_id,
                           "Only pending requests can be rejected.", ErrorRequestStatusInvalid);

            const uint128_t payer128FioAddHashed = fioreqctx_iter->payer_fio_address;

            // USED FOR MIGRATION
            const string payer_key = fioreqctx_iter->payer_key;
            const string payee_key = fioreqctx_iter->payee_key;
            // USED FOR MIGRATION

            const uint32_t present_time = now();

            auto namesbyname = fionames.get_index<"byname"_n>();
            auto fioname_iter = namesbyname.find(payer128FioAddHashed);

            fio_403_assert(fioname_iter != namesbyname.end(), ErrorSignature);

            const uint64_t account = fioname_iter->owner_account;
            const uint64_t payernameexp = fioname_iter->expiration;
            const string payerFioAddress = fioname_iter->name;
            FioAddress payerfa;
            getFioAddressStruct(payerFioAddress, payerfa);

            fio_400_assert(present_time <= payernameexp, "payer_fio_address", payerFioAddress,
                           "FIO Address expired", ErrorFioNameExpired);

            const uint128_t domHash = string_to_uint128_hash(payerfa.fiodomain.c_str());
            auto domainsbyname = domains.get_index<"byname"_n>();
            auto iterdom = domainsbyname.find(domHash);

            fio_400_assert(iterdom != domainsbyname.end(), "payer_fio_address", payerFioAddress,
                           "No such domain",
                           ErrorDomainNotRegistered);

            //add 30 days to the domain expiration, this call will work until 30 days past expire.
            const uint64_t domexp = get_time_plus_seconds(iterdom->expiration,SECONDS30DAYS);

            fio_400_assert(present_time <= domexp, "payer_fio_address", payerFioAddress,
                           "FIO Domain expired", ErrorFioNameExpired);

            const string payer_fio_address = fioname_iter->name;

            fio_403_assert(account == aactor.value, ErrorSignature);

            //begin fees, bundle eligible fee logic
            const uint128_t endpoint_hash = string_to_uint128_hash(REJECT_FUNDS_REQUEST_ENDPOINT);

            auto fees_by_endpoint = fiofees.get_index<"byendpoint"_n>();
            auto fee_iter = fees_by_endpoint.find(endpoint_hash);

            fio_400_assert(fee_iter != fees_by_endpoint.end(), "endpoint_name", REJECT_FUNDS_REQUEST_ENDPOINT,
                           "FIO fee not found for endpoint", ErrorNoEndpoint);

            const uint64_t fee_type = fee_iter->type;
            fio_400_assert(fee_type == 1, "fee_type", to_string(fee_type),
                           "reject_funds_request unexpected fee type for endpoint reject_funds_request, expected 1",
                           ErrorNoEndpoint);

            uint64_t fee_amount = 0;

            if (fioname_iter->bundleeligiblecountdown > 0) {
                action{
                        permission_level{_self, "active"_n},
                        AddressContract,
                        "decrcounter"_n,
                        make_tuple(payer_fio_address, 1)
                }.send();
            } else {
                fee_amount = fee_iter->suf_amount;
                fio_400_assert(max_fee >= (int64_t)fee_amount, "max_fee", to_string(max_fee),
                               "Fee exceeds supplied maximum.",
                               ErrorMaxFeeExceeded);

                fio_fees(aactor, asset(fee_amount, FIOSYMBOL), REJECT_FUNDS_REQUEST_ENDPOINT);
                process_rewards(tpid, fee_amount, get_self(), aactor);

                if (fee_amount > 0) {
                    INLINE_ACTION_SENDER(eosiosystem::system_contract, updatepower)
                            (SYSTEMACCOUNT, {{_self, "active"_n}},
                             {aactor, true}
                            );
                }
            }
            //end fees, bundle eligible fee logic
            fiorequestStatusTable.emplace(aactor, [&](struct fioreqsts &fr) {
                fr.id = fiorequestStatusTable.available_primary_key();;
                fr.fio_request_id = requestId;
                fr.status = static_cast<int64_t >(trxstatus::rejected);
                fr.metadata = "";
                fr.time_stamp = currentTime;
            });

            // USED FOR MIGRATION
            if(fioreqctx2_iter != trxtByRequestId.end()){
                trxtByRequestId.modify(fioreqctx2_iter, _self, [&](struct fiotrxt_info &fr) {
                    fr.fio_data_type = static_cast<int64_t >(trxstatus::rejected);
                    fr.obt_time = present_time;
                });
            }
            // USED FOR MIGRATION

            const string response_string = string("{\"status\": \"request_rejected\",\"fee_collected\":") +
                                     to_string(fee_amount) + string("}");

            if (REJECTFUNDSRAM > 0) {
                action(
                        permission_level{SYSTEMACCOUNT, "active"_n},
                        "eosio"_n,
                        "incram"_n,
                        std::make_tuple(aactor, REJECTFUNDSRAM)
                ).send();
            }

            fio_400_assert(transaction_size() <= MAX_TRX_SIZE, "transaction_size", std::to_string(transaction_size()),
              "Transaction is too large", ErrorTransactionTooLarge);

            send_response(response_string.c_str());
        }

    /********
        * this action will add a cancel status to the request for funds with the specified request id.
        * the input fiorequest id will be verified to ensure there is a request in the contexts table matching this id
        * before the status record is added to the index tables.
        * @param fio_request_id this is the id of the request in the fio request contexts table.
        * @param max_fee  this is the maximum fee that the sender of this transaction is willing to pay.
        * @param actor  this is the string representation of the FIO account that is associated with the signer of this tx.
        * @param tpid  this is the fio address of the domain owner associated with this request.
        */
    // @abi action
    [[eosio::action]]
    void cancelfndreq(
            const string &fio_request_id,
            const int64_t &max_fee,
            const string &actor,
            const string &tpid) {

        const name aactor = name(actor.c_str());
        require_auth(aactor);
        fio_400_assert(validateTPIDFormat(tpid), "tpid", tpid,
                       "TPID must be empty or valid FIO address",
                       ErrorPubKeyValid);
        fio_400_assert(max_fee >= 0, "max_fee", to_string(max_fee), "Invalid fee value",
                       ErrorMaxFeeInvalid);

        fio_400_assert(fio_request_id.length() > 0 && fio_request_id.length() < 16, "fio_request_id", fio_request_id, "No value specified",
                       ErrorRequestContextNotFound);

        const uint64_t currentTime = current_time();
        uint64_t requestId;

        requestId = std::atoi(fio_request_id.c_str());

        auto fioreqctx_iter = fiorequestContextsTable.find(requestId);

        // USED FOR MIGRATION
        auto trxtByRequestId = fioTransactionsTable.get_index<"byrequestid"_n>();
        auto fioreqctx2_iter = trxtByRequestId.find(requestId);
        // USED FOR MIGRATION

        fio_400_assert(fioreqctx_iter != fiorequestContextsTable.end(), "fio_request_id", fio_request_id,
                       "No such FIO Request", ErrorRequestContextNotFound);

        const uint128_t payee128FioAddHashed = fioreqctx_iter->payee_fio_address;

        // USED FOR MIGRATION
        const string payer_key = fioreqctx_iter->payer_key;
        const string payee_key = fioreqctx_iter->payee_key;
        // USED FOR MIGRATION

        const uint32_t present_time = now();

        //look for other statuses for this request.
        auto statusByRequestId = fiorequestStatusTable.get_index<"byfioreqid"_n>();
        auto fioreqstss_iter = statusByRequestId.find(requestId);
        fio_400_assert(fioreqstss_iter == statusByRequestId.end(), "fio_request_id", fio_request_id,
                       "Only pending requests can be cancelled.", ErrorRequestStatusInvalid);

        auto namesbyname = fionames.get_index<"byname"_n>();
        auto fioname_iter = namesbyname.find(payee128FioAddHashed);

        fio_403_assert(fioname_iter != namesbyname.end(), ErrorSignature);
        const uint64_t account = fioname_iter->owner_account;
        const uint64_t payeenameexp = fioname_iter->expiration;
        const string payeeFioAddress = fioname_iter->name;
        FioAddress payeefa;
        getFioAddressStruct(payeeFioAddress, payeefa);

        fio_400_assert(present_time <= payeenameexp, "payee_fio_address", payeeFioAddress,
                       "FIO Address expired", ErrorFioNameExpired);

        const uint128_t domHash = string_to_uint128_hash(payeefa.fiodomain.c_str());
        auto domainsbyname = domains.get_index<"byname"_n>();
        auto iterdom = domainsbyname.find(domHash);

        fio_400_assert(iterdom != domainsbyname.end(), "payee_fio_address", payeeFioAddress,
                       "No such domain",
                       ErrorDomainNotRegistered);

        //add 30 days to the domain expiration, this call will work until 30 days past expire.
        const uint64_t domexp = get_time_plus_seconds(iterdom->expiration,SECONDS30DAYS);

        fio_400_assert(present_time <= domexp, "payee_fio_address", payeeFioAddress,
                       "FIO Domain expired", ErrorFioNameExpired);

        const string payee_fio_address = fioname_iter->name;

        fio_403_assert(account == aactor.value, ErrorSignature);

        //begin fees, bundle eligible fee logic
        const uint128_t endpoint_hash = string_to_uint128_hash(CANCEL_FUNDS_REQUEST_ENDPOINT);

        auto fees_by_endpoint = fiofees.get_index<"byendpoint"_n>();
        auto fee_iter = fees_by_endpoint.find(endpoint_hash);

        fio_400_assert(fee_iter != fees_by_endpoint.end(), "endpoint_name", CANCEL_FUNDS_REQUEST_ENDPOINT,
                       "FIO fee not found for endpoint", ErrorNoEndpoint);

        const uint64_t fee_type = fee_iter->type;
        fio_400_assert(fee_type == 1, "fee_type", to_string(fee_type),
                       "cancel_funds_request unexpected fee type for endpoint cancel_funds_request, expected 1",
                       ErrorNoEndpoint);

        uint64_t fee_amount = 0;

        if (fioname_iter->bundleeligiblecountdown > 0) {
            action{
                    permission_level{_self, "active"_n},
                    AddressContract,
                    "decrcounter"_n,
                    make_tuple(payee_fio_address, 1)
            }.send();
        } else {
            fee_amount = fee_iter->suf_amount;
            fio_400_assert(max_fee >= (int64_t)fee_amount, "max_fee", to_string(max_fee),
                           "Fee exceeds supplied maximum.",
                           ErrorMaxFeeExceeded);

            fio_fees(aactor, asset(fee_amount, FIOSYMBOL), CANCEL_FUNDS_REQUEST_ENDPOINT);
            process_rewards(tpid, fee_amount, get_self(), aactor);

            if (fee_amount > 0) {
                INLINE_ACTION_SENDER(eosiosystem::system_contract, updatepower)
                        (SYSTEMACCOUNT, {{_self, "active"_n}},
                         {aactor, true}
                        );
            }
        }
        //end fees, bundle eligible fee logic

        fiorequestStatusTable.emplace(aactor, [&](struct fioreqsts &fr) {
            fr.id = fiorequestStatusTable.available_primary_key();;
            fr.fio_request_id = requestId;
            fr.status = static_cast<int64_t >(trxstatus::cancelled);
            fr.metadata = "";
            fr.time_stamp = currentTime;
        });

        // USED FOR MIGRATION
        if(fioreqctx2_iter != trxtByRequestId.end()){
            trxtByRequestId.modify(fioreqctx2_iter, _self, [&](struct fiotrxt_info &fr) {
                fr.fio_data_type = static_cast<int64_t >(trxstatus::cancelled);
                fr.obt_time = present_time;
            });
        }
        // USED FOR MIGRATION

        const string response_string = string("{\"status\": \"cancelled\",\"fee_collected\":") +
                                       to_string(fee_amount) + string("}");

        if (CANCELFUNDSRAM > 0) {
            action(
                    permission_level{SYSTEMACCOUNT, "active"_n},
                    "eosio"_n,
                    "incram"_n,
                    std::make_tuple(aactor, CANCELFUNDSRAM)
            ).send();
        }

        fio_400_assert(transaction_size() <= MAX_TRX_SIZE, "transaction_size", std::to_string(transaction_size()),
                       "Transaction is too large", ErrorTransactionTooLarge);

        send_response(response_string.c_str());
    }
};

    EOSIO_DISPATCH(FioRequestObt, (migrtrx)(recordobt)(newfundsreq)(rejectfndreq)(cancelfndreq))
}
