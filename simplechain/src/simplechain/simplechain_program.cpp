#include <simplechain/simplechain.h>
#include <iostream>
#include <simplechain/rpcserver.h>

using namespace simplechain;

int main(int argc, char** argv) {
	std::cout << "Hello, simplechain based on uvm" << std::endl;
	try {
		auto chain = std::make_shared<simplechain::blockchain>();

		// TODO: remove this demo code
		std::string contract1_addr;
		std::string caller_addr = std::string(SIMPLECHAIN_ADDRESS_PREFIX) + "caller1";
		
		{
			auto tx = std::make_shared<transaction>();
			auto op = operations_helper::mint(caller_addr, 0, 123);
			tx->tx_time = fc::time_point_sec(fc::time_point::now());
			tx->operations.push_back(op);
			
			chain->evaluate_transaction(tx);
			chain->accept_transaction_to_mempool(*tx);
		}
		{
			auto tx1 = std::make_shared<transaction>();
			std::string contract1_gpc_filepath("../test/test_contracts/token.gpc"); // TODO: load from command line arguments
			auto op = operations_helper::create_contract_from_file(caller_addr, contract1_gpc_filepath);
			tx1->operations.push_back(op);
			tx1->tx_time = fc::time_point_sec(fc::time_point::now());
			contract1_addr = op.calculate_contract_id();
			
			chain->evaluate_transaction(tx1);
			chain->accept_transaction_to_mempool(*tx1);
		}
		chain->generate_block();
		{
			auto tx = std::make_shared<transaction>();
			auto op = operations_helper::invoke_contract(caller_addr, contract1_addr, "init_token", { "test,TEST,10000,100" });
			tx->operations.push_back(op);
			tx->tx_time = fc::time_point_sec(fc::time_point::now());
			
			chain->evaluate_transaction(tx);
			chain->accept_transaction_to_mempool(*tx);
		}
		chain->generate_block();
		FC_ASSERT(chain->get_account_asset_balance(caller_addr, 0) == 123);
		FC_ASSERT(chain->get_contract_by_address(contract1_addr));
		auto state = chain->get_storage(contract1_addr, "state").as<std::string>();
		FC_ASSERT(state == "\"COMMON\"");

		RpcServer rpc_server(chain.get(), 8080);
		rpc_server.start();
	}
	catch (const std::exception& e) {
		std::cerr << e.what() << std::endl;
		return 1;
	}
	return 0;
}