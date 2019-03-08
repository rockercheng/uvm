#include <simplechain/native_contract.h>
#include <simplechain/blockchain.h>
#include <boost/algorithm/string.hpp>
#include <uvm/uvm_lib.h>
#include <jsondiff/jsondiff.h>

namespace simplechain {

#define THROW_CONTRACT_ERROR(...) FC_ASSERT(false, __VA_ARGS__)

	bool native_contract_interface::has_api(const std::string& api_name) const {
		const auto& api_names = apis();
		return api_names.find(api_name) != api_names.end();
	}

	using namespace cbor_diff;
	using namespace cbor;
	using namespace std;
	// TODO: use fast_map to store users, allowed of token contract

	void native_contract_interface::set_contract_storage(const std::string& contract_address, const std::string& storage_name, const StorageDataType& value)
	{
		if (_contract_invoke_result.storage_changes.find(contract_address) == _contract_invoke_result.storage_changes.end())
		{
			_contract_invoke_result.storage_changes[contract_address] = contract_storage_changes_type();
		}
		auto& storage_changes = _contract_invoke_result.storage_changes[contract_address];
		if (storage_changes.find(storage_name) == storage_changes.end())
		{
			StorageDataChangeType change;
			change.after = value;
			const auto &before = _evaluate->get_storage(contract_address, storage_name);
			cbor_diff::CborDiff differ;
			const auto& before_cbor = cbor_decode(before.storage_data);
			const auto& after_cbor = cbor_decode(change.after.storage_data);
			auto diff = differ.diff(before_cbor, after_cbor);
			change.storage_diff.storage_data = cbor_encode(diff->value());
			change.before = before;
			storage_changes[storage_name] = change;
		}
		else
		{
			auto& change = storage_changes[storage_name];
			auto before = change.before;
			auto after = value;
			change.after = after;
			cbor_diff::CborDiff differ;
			const auto& before_cbor = cbor_diff::cbor_decode(before.storage_data);
			const auto& after_cbor = cbor_diff::cbor_decode(after.storage_data);
			auto diff = differ.diff(before_cbor, after_cbor);
			change.storage_diff.storage_data = cbor_encode(diff->value());
		}
	}

	void native_contract_interface::set_contract_storage(const std::string& contract_address, const std::string& storage_name, cbor::CborObjectP cbor_value) {
		StorageDataType value;
		value.storage_data = cbor_encode(cbor_value);
		return set_contract_storage(contract_address, storage_name, value);
	}

	StorageDataType native_contract_interface::get_contract_storage(const std::string& contract_address, const std::string& storage_name)
	{
		if (_contract_invoke_result.storage_changes.find(contract_address) == _contract_invoke_result.storage_changes.end())
		{
			return _evaluate->get_storage(contract_address, storage_name);
		}
		auto& storage_changes = _contract_invoke_result.storage_changes[contract_address];
		if (storage_changes.find(storage_name) == storage_changes.end())
		{
			return _evaluate->get_storage(contract_address, storage_name);
		}
		return storage_changes[storage_name].after;
	}

	void native_contract_interface::emit_event(const std::string& contract_address, const std::string& event_name, const std::string& event_arg)
	{
		FC_ASSERT(!event_name.empty());
		contract_event_notify_info info;
		info.event_name = event_name;
		info.event_arg = event_arg;
		//info.caller_addr = caller_address->address_to_string();
		info.block_num = 1 + _evaluate->get_chain()->head_block_number();

		_contract_invoke_result.events.push_back(info);
	}


	// token contract
	std::string token_native_contract::contract_key() const
	{
		return token_native_contract::native_contract_key();
	}
	address token_native_contract::contract_address() const {
		return contract_id;
	}
	std::set<std::string> token_native_contract::apis() const {
		return { "init", "init_token", "transfer", "transferFrom", "balanceOf", "approve", "approvedBalanceFrom", "allApprovedFromUser", "state", "supply", "precision", "tokenName", "tokenSymbol" };
	}
	std::set<std::string> token_native_contract::offline_apis() const {
		return { "balanceOf", "approvedBalanceFrom", "allApprovedFromUser", "state", "supply", "precision", "tokenName", "tokenSymbol" };
	}
	std::set<std::string> token_native_contract::events() const {
		return { "Inited", "Transfer", "Approved" };
	}

	static const string not_inited_state_of_token_contract = "NOT_INITED";
	static const string common_state_of_token_contract = "COMMON";

	contract_invoke_result token_native_contract::init_api(const std::string& api_name, const std::string& api_arg)
	{
		set_contract_storage(contract_id, string("name"), CborObject::from_string(""));
		set_contract_storage(contract_id, string("symbol"), CborObject::from_string(""));
		set_contract_storage(contract_id, string("supply"), CborObject::from_int(0));
		set_contract_storage(contract_id, string("precision"), CborObject::from_int(0));
		set_contract_storage(contract_id, string("users"), CborObject::create_map(0));
		set_contract_storage(contract_id, string("allowed"), CborObject::create_map(0));
		set_contract_storage(contract_id, string("state"), CborObject::from_string(not_inited_state_of_token_contract));
		auto caller_addr = _evaluate->caller_address;
		FC_ASSERT(!caller_addr.empty());
		set_contract_storage(contract_id, string("admin"), CborObject::from_string(caller_addr));
		return _contract_invoke_result;
	}

	string token_native_contract::check_admin()
	{
		auto caller_addr = _evaluate->caller_address;
		auto admin_storage = get_contract_storage(contract_id, string("admin"));
		auto admin = cbor_diff::cbor_decode(admin_storage.storage_data);
		if (admin->is_string() && admin->as_string() == caller_addr)
			return admin->as_string();
		THROW_CONTRACT_ERROR("only admin can call this api");
	}

	string token_native_contract::get_storage_state()
	{
		auto state_storage = get_contract_storage(contract_id, string("state"));
		auto state = cbor_decode(state_storage.storage_data);
		return state->as_string();
	}

	string token_native_contract::get_storage_token_name()
	{
		auto name_storage = get_contract_storage(contract_id, string("name"));
		auto name = cbor_decode(name_storage.storage_data);
		return name->as_string();
	}

	string token_native_contract::get_storage_token_symbol()
	{
		auto symbol_storage = get_contract_storage(contract_id, string("symbol"));
		auto symbol = cbor_decode(symbol_storage.storage_data);
		return symbol->as_string();
	}


	int64_t token_native_contract::get_storage_supply()
	{
		auto supply_storage = get_contract_storage(contract_id, string("supply"));
		auto supply = cbor_decode(supply_storage.storage_data);
		return supply->force_as_int();
	}
	int64_t token_native_contract::get_storage_precision()
	{
		auto precision_storage = get_contract_storage(contract_id, string("precision"));
		auto precision = cbor_decode(precision_storage.storage_data);
		return precision->force_as_int();
	}

	cbor::CborMapValue token_native_contract::get_storage_users()
	{
		auto users_storage = get_contract_storage(contract_id, string("users"));
		auto users = cbor_decode(users_storage.storage_data);
		return users->as_map();
	}

	cbor::CborMapValue token_native_contract::get_storage_allowed()
	{
		auto allowed_storage = get_contract_storage(contract_id, string("allowed"));
		auto allowed = cbor_decode(allowed_storage.storage_data);
		return allowed->as_map();
	}

	int64_t token_native_contract::get_balance_of_user(const string& owner_addr)
	{
		const auto& users = get_storage_users();
		if (users.find(owner_addr) == users.end())
			return 0;
		auto user_balance_cbor = users.at(owner_addr);
		return user_balance_cbor->force_as_int();
	}

	std::string token_native_contract::get_from_address()
	{
		return _evaluate->caller_address; // FIXME: when get from_address, caller maybe other contract
	}

	static bool is_numeric(std::string number)
	{
		char* end = 0;
		std::strtod(number.c_str(), &end);

		return end != 0 && *end == 0;
	}


	static bool is_integral(std::string number)
	{
		return is_numeric(number.c_str()) && std::strchr(number.c_str(), '.') == 0;
	}

	// arg format: name,symbol,supply,precision
	contract_invoke_result token_native_contract::init_token_api(const std::string& api_name, const std::string& api_arg)
	{
		check_admin();
		if (get_storage_state() != not_inited_state_of_token_contract)
			THROW_CONTRACT_ERROR("this token contract inited before");
		std::vector<string> parsed_args;
		boost::split(parsed_args, api_arg, [](char c) {return c == ','; });
		if (parsed_args.size() < 4)
			THROW_CONTRACT_ERROR("argument format error, need format: name,symbol,supply,precision");
		string name = parsed_args[0];
		boost::trim(name);
		string symbol = parsed_args[1];
		boost::trim(symbol);
		if (name.empty() || symbol.empty())
			THROW_CONTRACT_ERROR("argument format error, need format: name,symbol,supply,precision");
		string supply_str = parsed_args[2];
		if (!is_integral(supply_str))
			THROW_CONTRACT_ERROR("argument format error, need format: name,symbol,supply,precision");
		int64_t supply = std::stoll(supply_str);
		if (supply <= 0)
			THROW_CONTRACT_ERROR("argument format error, supply must be positive integer");
		string precision_str = parsed_args[3];
		if (!is_integral(precision_str))
			THROW_CONTRACT_ERROR("argument format error, need format: name,symbol,supply,precision");
		int64_t precision = std::stoll(precision_str);
		if (precision <= 0)
			THROW_CONTRACT_ERROR("argument format error, precision must be positive integer");
		// allowedPrecisions = [1,10,100,1000,10000,100000,1000000,10000000,100000000]
		std::vector<int64_t> allowed_precisions = { 1,10,100,1000,10000,100000,1000000,10000000,100000000 };
		if (std::find(allowed_precisions.begin(), allowed_precisions.end(), precision) == allowed_precisions.end())
			THROW_CONTRACT_ERROR("argument format error, precision must be any one of [1,10,100,1000,10000,100000,1000000,10000000,100000000]");
		set_contract_storage(contract_id, string("state"), CborObject::from_string(common_state_of_token_contract));
		set_contract_storage(contract_id, string("precision"), CborObject::from_int(precision));
		set_contract_storage(contract_id, string("supply"), CborObject::from_int(supply));
		set_contract_storage(contract_id, string("name"), CborObject::from_string(name));
		set_contract_storage(contract_id, "symbol", CborObject::from_string(symbol));

		cbor::CborMapValue users;
		auto caller_addr = _evaluate->caller_address;
		users[caller_addr] = cbor::CborObject::from_int(supply);
		set_contract_storage(contract_id, string("users"), CborObject::create_map(users));
		emit_event(contract_id, "Inited", supply_str);
		return _contract_invoke_result;
	}

	contract_invoke_result token_native_contract::balance_of_api(const std::string& api_name, const std::string& api_arg)
	{
		if (get_storage_state() != common_state_of_token_contract)
			THROW_CONTRACT_ERROR("this token contract state doesn't allow transfer");
		std::string owner_addr = api_arg;
		auto amount = get_balance_of_user(owner_addr);
		_contract_invoke_result.api_result = std::to_string(amount);
		return _contract_invoke_result;
	}

	contract_invoke_result token_native_contract::state_api(const std::string& api_name, const std::string& api_arg)
	{
		const auto& state = get_storage_state();
		_contract_invoke_result.api_result = state;
		return _contract_invoke_result;
	}

	contract_invoke_result token_native_contract::token_name_api(const std::string& api_name, const std::string& api_arg)
	{
		const auto& token_name = get_storage_token_name();
		_contract_invoke_result.api_result = token_name;
		return _contract_invoke_result;
	}

	contract_invoke_result token_native_contract::token_symbol_api(const std::string& api_name, const std::string& api_arg)
	{
		const auto& token_symbol = get_storage_token_symbol();
		_contract_invoke_result.api_result = token_symbol;
		return _contract_invoke_result;
	}

	contract_invoke_result token_native_contract::supply_api(const std::string& api_name, const std::string& api_arg)
	{
		auto supply = get_storage_supply();
		_contract_invoke_result.api_result = std::to_string(supply);
		return _contract_invoke_result;
	}
	contract_invoke_result token_native_contract::precision_api(const std::string& api_name, const std::string& api_arg)
	{
		auto precision = get_storage_precision();
		_contract_invoke_result.api_result = std::to_string(precision);
		return _contract_invoke_result;
	}

	contract_invoke_result token_native_contract::approved_balance_from_api(const std::string& api_name, const std::string& api_arg)
	{
		printf("approved_balance_from_api arg: %s\n", api_arg.c_str());
		if (get_storage_state() != common_state_of_token_contract)
			THROW_CONTRACT_ERROR("this token contract state doesn't allow this api");
		auto allowed = get_storage_allowed();
		std::vector<string> parsed_args;
		boost::split(parsed_args, api_arg, [](char c) {return c == ','; });
		if (parsed_args.size() < 2)
			THROW_CONTRACT_ERROR("argument format error, need format: spenderAddress, authorizerAddress");
		string spender_address = parsed_args[0];
		boost::trim(spender_address);
		string authorizer_address = parsed_args[1];
		boost::trim(authorizer_address);
		int64_t approved_amount = 0;
		if (allowed.find(authorizer_address) != allowed.end())
		{
			auto allowed_data = allowed[authorizer_address]->as_map();
			if (allowed_data.find(spender_address) != allowed_data.end())
			{
				approved_amount = allowed_data[spender_address]->force_as_int();
			}
		}

		_contract_invoke_result.api_result = std::to_string(approved_amount);
		return _contract_invoke_result;
	}
	contract_invoke_result token_native_contract::all_approved_from_user_api(const std::string& api_name, const std::string& api_arg)
	{
		printf("enter all_approved_from_user_api\n");
		if (get_storage_state() != common_state_of_token_contract)
			THROW_CONTRACT_ERROR("this token contract state doesn't allow this api");
		auto allowed = get_storage_allowed();
		printf("allowed got\n");
		string from_address = api_arg;
		boost::trim(from_address);

		cbor::CborMapValue allowed_data;
		if (allowed.find(from_address) != allowed.end())
		{
			allowed_data = allowed[from_address]->as_map();
		}
		printf("to parse json\n");
		auto L = uvm::lua::lib::create_lua_state(true); // FIXME: don't use L here
		UvmStateValue usv;
		usv.pointer_value = _evaluate;
		uvm::lua::lib::set_lua_state_value(L, "native_register_evaluate_state", usv, LUA_STATE_VALUE_POINTER);
		auto allowed_data_cbor = CborObject::create_map(allowed_data);
		printf("allowed_data_cbor cbor type: %d\n", allowed_data_cbor->object_type());
		auto allowed_data_uvm_storage = cbor_to_uvm_storage_value(L, allowed_data_cbor.get());
		printf("allowed_data_uvm_storage generated\n");
		printf("allowed_data_uvm_storage type: %d\n", allowed_data_uvm_storage.type);
		auto allowed_data_json = simplechain::uvm_storage_value_to_json(allowed_data_uvm_storage); // FIXME
		printf("allowed_data_json generated\n");
		try {
			if (allowed_data_json.is_object()) {
				printf("found json object\n");
				auto obj = allowed_data_json.as<jsondiff::JsonObject>();
				printf("obj size: %d\n", obj.size());
				for (auto it = obj.begin(); it != obj.end(); it++)
				{
					printf("found key\n");
					printf("key: %s\n", it->key().c_str());

				}
			}
			printf(fc::json::to_string(allowed_data_json, fc::json::legacy_generator).c_str());
		}
		catch (const std::exception& e) {
			printf("to json string error: %s\n", e.what());
		}
		auto allowed_data_str = jsondiff::json_dumps(allowed_data_json);
		uvm::lua::lib::close_lua_state(L);
		printf("json parsed\n");
		_contract_invoke_result.api_result = allowed_data_str;
		return _contract_invoke_result;
	}

	contract_invoke_result token_native_contract::transfer_api(const std::string& api_name, const std::string& api_arg)
	{
		if (get_storage_state() != common_state_of_token_contract)
			THROW_CONTRACT_ERROR("this token contract state doesn't allow transfer");
		std::vector<string> parsed_args;
		boost::split(parsed_args, api_arg, [](char c) {return c == ','; });
		if (parsed_args.size() < 2)
			THROW_CONTRACT_ERROR("argument format error, need format: toAddress,amount(with precision, integer)");
		string to_address = parsed_args[0];
		boost::trim(to_address);
		string amount_str = parsed_args[1];
		boost::trim(amount_str);
		if (!is_integral(amount_str))
			THROW_CONTRACT_ERROR("argument format error, amount must be positive integer");
		int64_t amount = std::stoll(amount_str);
		if (amount <= 0)
			THROW_CONTRACT_ERROR("argument format error, amount must be positive integer");

		string from_addr = get_from_address();
		auto users = get_storage_users();
		if (users.find(from_addr) == users.end() || users[from_addr]->force_as_int() < amount)
			THROW_CONTRACT_ERROR("you have not enoungh amount to transfer out");
		auto from_addr_remain = users[from_addr]->force_as_int() - amount;
		if (from_addr_remain > 0)
			users[from_addr] = CborObject::from_int(from_addr_remain);
		else
			users.erase(from_addr);
		int64_t to_amount = 0;
		if (users.find(to_address) != users.end())
			to_amount = users[to_address]->force_as_int();
		users[to_address] = CborObject::from_int(to_amount + amount);
		set_contract_storage(contract_id, string("users"), CborObject::create_map(users));
		jsondiff::JsonObject event_arg;
		event_arg["from"] = from_addr;
		event_arg["to"] = to_address;
		event_arg["amount"] = amount;
		emit_event(contract_id, "Transfer", jsondiff::json_dumps(event_arg));
		return _contract_invoke_result;
	}

	contract_invoke_result token_native_contract::approve_api(const std::string& api_name, const std::string& api_arg)
	{
		if (get_storage_state() != common_state_of_token_contract)
			THROW_CONTRACT_ERROR("this token contract state doesn't allow approve");
		std::vector<string> parsed_args;
		boost::split(parsed_args, api_arg, [](char c) {return c == ','; });
		if (parsed_args.size() < 2)
			THROW_CONTRACT_ERROR("argument format error, need format: spenderAddress, amount(with precision, integer)");
		string spender_address = parsed_args[0];
		boost::trim(spender_address);
		string amount_str = parsed_args[1];
		boost::trim(amount_str);
		if (!is_integral(amount_str))
			THROW_CONTRACT_ERROR("argument format error, amount must be positive integer");
		int64_t amount = std::stoll(amount_str);
		if (amount <= 0)
			THROW_CONTRACT_ERROR("argument format error, amount must be positive integer");
		auto allowed = get_storage_allowed();
		cbor::CborMapValue allowed_data;
		std::string contract_caller = get_from_address();
		if (allowed.find(contract_caller) == allowed.end())
			allowed_data = cbor::CborMapValue();
		else
		{
			allowed_data = allowed[contract_caller]->as_map();
		}
		allowed_data[spender_address] = CborObject::from_int(amount);
		allowed[contract_caller] = CborObject::create_map(allowed_data);
		set_contract_storage(contract_id, string("allowed"), CborObject::create_map(allowed));
		jsondiff::JsonObject event_arg;
		event_arg["from"] = contract_caller;
		event_arg["spender"] = spender_address;
		event_arg["amount"] = amount;
		emit_event(contract_id, "Approved", jsondiff::json_dumps(event_arg));
		return _contract_invoke_result;
	}

	contract_invoke_result token_native_contract::transfer_from_api(const std::string& api_name, const std::string& api_arg)
	{
		if (get_storage_state() != common_state_of_token_contract)
			THROW_CONTRACT_ERROR("this token contract state doesn't allow transferFrom");
		std::vector<string> parsed_args;
		boost::split(parsed_args, api_arg, [](char c) {return c == ','; });
		if (parsed_args.size() < 3)
			THROW_CONTRACT_ERROR("argument format error, need format:fromAddress, toAddress, amount(with precision, integer)");
		string from_address = parsed_args[0];
		boost::trim(from_address);
		string to_address = parsed_args[1];
		boost::trim(to_address);
		string amount_str = parsed_args[2];
		boost::trim(amount_str);
		if (!is_integral(amount_str))
			THROW_CONTRACT_ERROR("argument format error, amount must be positive integer");
		int64_t amount = std::stoll(amount_str);
		if (amount <= 0)
			THROW_CONTRACT_ERROR("argument format error, amount must be positive integer");

		auto users = get_storage_users();
		auto allowed = get_storage_allowed();
		if (get_balance_of_user(from_address) < amount)
		{
			THROW_CONTRACT_ERROR("fromAddress not have enough token to withdraw");
		}
		cbor::CborMapValue allowed_data;
		if (allowed.find(from_address) == allowed.end())
			THROW_CONTRACT_ERROR("not enough approved amount to withdraw");
		else
		{
			allowed_data = allowed[from_address]->as_map();
		}
		auto contract_caller = get_from_address();
		if (allowed_data.find(contract_caller) == allowed_data.end())
			THROW_CONTRACT_ERROR("not enough approved amount to withdraw");
		auto approved_amount = allowed_data[contract_caller]->force_as_int();
		if (approved_amount < amount)
			THROW_CONTRACT_ERROR("not enough approved amount to withdraw");
		auto from_addr_remain = users[from_address]->force_as_int() - amount;
		if (from_addr_remain > 0)
			users[from_address] = cbor::CborObject::from_int(from_addr_remain);
		else
			users.erase(from_address);
		int64_t to_amount = 0;
		if (users.find(to_address) != users.end())
			to_amount = users[to_address]->force_as_int();
		users[to_address] = cbor::CborObject::from_int(to_amount + amount);
		set_contract_storage(contract_id, string("users"), CborObject::create_map(users));

		allowed_data[contract_caller] = cbor::CborObject::from_int(approved_amount - amount);
		if (allowed_data[contract_caller]->force_as_int() == 0)
			allowed_data.erase(contract_caller);
		allowed[from_address] = CborObject::create_map(allowed_data);
		set_contract_storage(contract_id, string("allowed"), CborObject::create_map(allowed));

		jsondiff::JsonObject event_arg;
		event_arg["from"] = from_address;
		event_arg["to"] = to_address;
		event_arg["amount"] = amount;
		emit_event(contract_id, "Transfer", jsondiff::json_dumps(event_arg));

		return _contract_invoke_result;
	}

	contract_invoke_result token_native_contract::invoke(const std::string& api_name, const std::string& api_arg) {
		std::map<std::string, std::function<contract_invoke_result(const std::string&, const std::string&)>> apis = {
			{"init", std::bind(&token_native_contract::init_api, this, std::placeholders::_1, std::placeholders::_2)},
			{"init_token", std::bind(&token_native_contract::init_token_api, this, std::placeholders::_1, std::placeholders::_2)},
			{"transfer", std::bind(&token_native_contract::transfer_api, this, std::placeholders::_1, std::placeholders::_2)},
			{"transferFrom", std::bind(&token_native_contract::transfer_from_api, this, std::placeholders::_1, std::placeholders::_2)},
			{"balanceOf", std::bind(&token_native_contract::balance_of_api, this, std::placeholders::_1, std::placeholders::_2)},
			{"approve", std::bind(&token_native_contract::approve_api, this, std::placeholders::_1, std::placeholders::_2)},
			{"approvedBalanceFrom", std::bind(&token_native_contract::approved_balance_from_api, this, std::placeholders::_1, std::placeholders::_2)},
			{"allApprovedFromUser", std::bind(&token_native_contract::all_approved_from_user_api, this, std::placeholders::_1, std::placeholders::_2)},
			{"state", std::bind(&token_native_contract::state_api, this, std::placeholders::_1, std::placeholders::_2)},
			{"supply", std::bind(&token_native_contract::supply_api, this, std::placeholders::_1, std::placeholders::_2)},
			{"precision", std::bind(&token_native_contract::precision_api, this, std::placeholders::_1, std::placeholders::_2)},
			{"tokenName", std::bind(&token_native_contract::token_name_api, this, std::placeholders::_1, std::placeholders::_2)},
			{"tokenSymbol", std::bind(&token_native_contract::token_symbol_api, this, std::placeholders::_1, std::placeholders::_2)},
		};
		if (apis.find(api_name) != apis.end())
		{
			printf("token native api: %s\n, arg: %s\n", api_name.c_str(), api_arg.c_str());
			contract_invoke_result res = apis[api_name](api_name, api_arg);
			res.invoker = _evaluate->caller_address;
			return res;
		}
		THROW_CONTRACT_ERROR("token api not found");
	}

	bool native_contract_finder::has_native_contract_with_key(const std::string& key)
	{
		std::vector<std::string> native_contract_keys = {
			// demo_native_contract::native_contract_key(),
			token_native_contract::native_contract_key()
		};
		return std::find(native_contract_keys.begin(), native_contract_keys.end(), key) != native_contract_keys.end();
	}
	shared_ptr<native_contract_interface> native_contract_finder::create_native_contract_by_key(evaluate_state* evaluate, const std::string& key, const address& contract_address)
	{
		/*if (key == demo_native_contract::native_contract_key())
		{
			return std::make_shared<demo_native_contract>(evaluate, contract_address);
		}
		else */
		if (key == token_native_contract::native_contract_key())
		{
			return std::make_shared<token_native_contract>(evaluate, contract_address);
		}
		else
		{
			return nullptr;
		}
	}


}