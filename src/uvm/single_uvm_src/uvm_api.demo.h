﻿#ifndef uvm_lua_api_demo_h
#define uvm_lua_api_demo_h

#include <glua/lprefix.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string>
#include <sstream>
#include <utility>
#include <list>
#include <map>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <glua/glua_api.h>
#include <glua/glua_lib.h>
#include <glua/glua_lutil.h>
#include <glua/lobject.h>
#include <glua/lstate.h>


namespace uvm {
  namespace lua {
    namespace api {
      // demo，API

      class DemoGluaChainApi : public IGluaChainApi
      {
      public:
        /**
        * check whether the contract apis limit over, in this lua_State
        * @param L the lua stack
        * @return TRUE(1 or not 0) if over limit(will break the vm), FALSE(0) if not over limit
        */
        virtual int check_contract_api_instructions_over_limit(lua_State *L);

        /**
        * whether exception happen in L
        */
        virtual bool has_exception(lua_State *L);

        /**
        * clear exception marked
        */
        virtual void clear_exceptions(lua_State *L);


        /**
        * when exception happened, use this api to tell uvm
        * @param L the lua stack
        * @param code error code, 0 is OK, other is different error
        * @param error_format error info string, will be released by lua
        * @param ... error arguments
        */
        virtual void throw_exception(lua_State *L, int code, const char *error_format, ...);

        /**
        * get contract info stored before from uvm api
        * @param name contract name
        * @param contract_info_ret this api save the contract's api name array here if found, this var will be free by this api
        * @return TRUE(1 or not 0) if success, FALSE(0) if failed
        */
        virtual int get_stored_contract_info(lua_State *L, const char *name, std::shared_ptr<GluaContractInfo> contract_info_ret);

        virtual int get_stored_contract_info_by_address(lua_State *L, const char *address, std::shared_ptr<GluaContractInfo> contract_info_ret);

        virtual std::shared_ptr<GluaModuleByteStream> get_bytestream_from_code(lua_State *L, const uvm::blockchain::Code& code);
        /**
        * load contract lua byte stream from uvm api
        */
        virtual std::shared_ptr<GluaModuleByteStream> open_contract(lua_State *L, const char *name);

        virtual std::shared_ptr<GluaModuleByteStream> open_contract_by_address(lua_State *L, const char *address);

        /**
        * get contract address/id from uvm by contract name
        */
        virtual void get_contract_address_by_name(lua_State *L, const char *name, char *address, size_t *address_size);

        /*
        * check whether the contract exist
        */
        virtual bool check_contract_exist(lua_State *L, const char *name);

        /**
        * check contract exist by ID string address
        */
        virtual bool check_contract_exist_by_address(lua_State *L, const char *address);

        /**
        * store contract lua module byte stream to uvm api
        */
        //int save_contract(lua_State *L, const char *name, GluaModuleByteStream *stream);

        /**
        * register new storage name of contract to uvm
        */
        virtual bool register_storage(lua_State *L, const char *contract_name, const char *name);

        /**
        * directly get storage value from uvm
        */
        //void free_contract_storage(lua_State *L, GluaStorageValue* storage);

        virtual GluaStorageValue get_storage_value_from_uvm(lua_State *L, const char *contract_name, std::string name);

        virtual GluaStorageValue get_storage_value_from_uvm_by_address(lua_State *L, const char *contract_address, std::string name);

        /**
        * after lua merge storage changes in lua_State, use the function to store the merged changes of storage to uvm
        */
        virtual bool commit_storage_changes_to_uvm(lua_State *L, AllContractsChangesMap &changes);

        /**
        * lua_State（），
        * lua_State
        * glua/，，lightuserdataglua
        */
        virtual intptr_t register_object_in_pool(lua_State *L, intptr_t object_addr, GluaOutsideObjectTypes type);

        /**
        * (register_object_in_pool，)lua_State（)
        * ，，0
        */
        virtual intptr_t is_object_in_pool(lua_State *L, intptr_t object_key, GluaOutsideObjectTypes type);

        /**
        * lua_State
        */
        virtual void release_objects_in_pool(lua_State *L);

        /************************************************************************/
        /* transfer asset from contract by account address                      */
        /************************************************************************/
        virtual lua_Integer transfer_from_contract_to_address(lua_State *L, const char *contract_address, const char *to_address,
          const char *asset_type, int64_t amount);

        /************************************************************************/
        /* transfer asset from contract by account name on chain                */
        /************************************************************************/
        virtual lua_Integer transfer_from_contract_to_public_account(lua_State *L, const char *contract_address, const char *to_account_name,
          const char *asset_type, int64_t amount);

        virtual int64_t get_contract_balance_amount(lua_State *L, const char *contract_address, const char* asset_symbol);
        virtual int64_t get_transaction_fee(lua_State *L);
        virtual uint32_t get_chain_now(lua_State *L);
        virtual uint32_t get_chain_random(lua_State *L);
        virtual std::string get_transaction_id(lua_State *L);
        virtual uint32_t get_header_block_num(lua_State *L);
        virtual uint32_t wait_for_future_random(lua_State *L, int next);

        virtual int32_t get_waited(lua_State *L, uint32_t num);

        virtual void emit(lua_State *L, const char* contract_id, const char* event_name, const char* event_param);
		virtual bool is_valid_address(lua_State *L, const char *address_str);
		virtual const char *get_system_asset_symbol(lua_State *L);

		virtual std::string get_address_role(lua_State* L, const std::string& addr);

      };

    }
  }
}


#endif