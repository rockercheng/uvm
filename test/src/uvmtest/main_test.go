package main

import (
	"bytes"
	"time"

	// "crypto/ecdsa"
	// "crypto/sha256"
	// "encoding/hex"
	"encoding/json"
	"errors"
	"fmt"

	// "io/ioutil"
	// "log"
	// "math/rand"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"

	// "reflect"
	"runtime"
	// "strconv"
	// "strings"
	"testing"

	"github.com/bitly/go-simplejson"
	"github.com/stretchr/testify/assert"
	// "github.com/zoowii/ecdsatools"
	// gosmt "github.com/zoowii/go_sparse_merkle_tree"
)

func findSimpleChainPath() string {
	dir, err := os.Getwd()
	if err != nil {
		panic(err)
	}
	dirAbs, err := filepath.Abs(dir)
	if err != nil {
		panic(err)
	}
	uvmDir := filepath.Dir(filepath.Dir(filepath.Dir(dirAbs)))
	if runtime.GOOS == "windows" {
		return filepath.Join(uvmDir, "x64", "Debug", "simplechain.exe")
	}
	return filepath.Join(uvmDir, "simplechain_runner")
}

func findUvmCompilerPath() string {
	dir, err := os.Getwd()
	if err != nil {
		panic(err)
	}
	dirAbs, err := filepath.Abs(dir)
	if err != nil {
		panic(err)
	}
	uvmDir := filepath.Dir(filepath.Dir(filepath.Dir(dirAbs)))
	if runtime.GOOS == "windows" {
		return filepath.Join(uvmDir, "test", "uvm_compiler.exe")
	}
	return filepath.Join(uvmDir, "test", "uvm_compiler")
}

func findUvmAssPath() string {
	dir, err := os.Getwd()
	if err != nil {
		panic(err)
	}
	dirAbs, err := filepath.Abs(dir)
	if err != nil {
		panic(err)
	}
	uvmDir := filepath.Dir(filepath.Dir(filepath.Dir(dirAbs)))
	if runtime.GOOS == "windows" {
		return filepath.Join(uvmDir, "test", "uvm_ass.exe")
	}
	return filepath.Join(uvmDir, "test", "uvm_ass")
}

var uvmCompilerPath = findUvmCompilerPath()
var uvmAssPath = findUvmAssPath()
var simpleChainPath = findSimpleChainPath()
var simpleChainDefaultPort = 8080

func execCommand(program string, args ...string) (string, string) {
	cmd := exec.Command(program, args...)
	var outb, errb bytes.Buffer
	cmd.Stdin = os.Stdin
	cmd.Stdout = &outb
	cmd.Stderr = &errb
	err := cmd.Run()
	if err != nil {
		fmt.Printf("%v\n", err)
	}
	return outb.String(), errb.String()
}

func execCommandBackground(program string, args ...string) *exec.Cmd {
	cmd := exec.Command(program, args...)
	var outb, errb bytes.Buffer
	cmd.Stdin = os.Stdin
	cmd.Stdout = &outb
	cmd.Stderr = &errb
	err := cmd.Start()
	if err != nil {
		fmt.Printf("%v\n", err)
	}
	return cmd
}

func kill(cmd *exec.Cmd) error {
	return cmd.Process.Kill()
	// kill := exec.Command("TASKKILL", "/T", "/F", "/PID", strconv.Itoa(cmd.Process.Pid)) // TODO: when in linux
	// kill.Stderr = os.Stderr
	// kill.Stdout = os.Stdout
	// return kill.Run()
}

type rpcRequest struct {
	Method string        `json:"method"`
	Params []interface{} `json:"params"`
}

func simpleChainRPC(method string, params ...interface{}) (*simplejson.Json, error) {
	reqObj := rpcRequest{Method: method, Params: params}
	reqBytes, err := json.Marshal(reqObj)
	if err != nil {
		return nil, err
	}
	url := "http://localhost:8080/api"
	httpRes, err := http.Post(url, "application/json", bytes.NewReader(reqBytes))
	if err != nil {
		return nil, err
	}
	if httpRes.StatusCode != 200 {
		return nil, errors.New("rpc error of " + httpRes.Status)
	}
	resJSON, err := simplejson.NewFromReader(httpRes.Body)
	if err != nil {
		return nil, err
	}
	if method == "get_tx_receipt" {
		resJSONBytes, err := json.Marshal(resJSON)
		if err != nil {
			return nil, err
		}
		println("get_tx_receipt res: ", string(resJSONBytes))
	}

	// resJSONBytes, err := json.Marshal(resJSON)
	// if err != nil {
	// 	return nil, err
	// }
	// println("res: ", string(resJSONBytes))
	// fmt.Printf("res: %s\n", string(resJSONBytes))

	return resJSON.Get("result"), nil
}

func testContractPath(contractPath string) string {
	dir, err := os.Getwd()
	if err != nil {
		panic(err)
	}
	dirAbs, err := filepath.Abs(dir)
	if err != nil {
		panic(err)
	}
	uvmDir := filepath.Dir(filepath.Dir(filepath.Dir(dirAbs)))
	return filepath.Join(uvmDir, "test", "test_contracts", contractPath)
}

func getAccountBalanceOfAssetID(caller string, assetID int) (int, error) {
	res, err := simpleChainRPC("get_account_balances", caller)
	if err != nil {
		return 0, err
	}
	return res.GetIndex(assetID).GetIndex(1).MustInt(), nil
}

type TxEvent struct {
	EventName string `json:"event_name"`
	EventArg  string `json:"event_arg"`
}

func getTxReceiptEvents(txid string) (result []*TxEvent, err error) {
	result = make([]*TxEvent, 0)
	res, err := simpleChainRPC("get_tx_receipt", txid)
	if err != nil {
		return
	}
	eventsArrayJson := res.Get("events")
	eventsBytes, err := eventsArrayJson.Encode()
	if err != nil {
		return
	}
	err = json.Unmarshal(eventsBytes, &result)
	if err != nil {
		return
	}
	return
}

func testDeployDicewin(t *testing.T) {
	caller1 := "SPLtest1"
	var res *simplejson.Json
	var err error

	// 0
	// compileOut1, compileErr1 := execCommand(uvmCompilerPath, "-g", testContractPath("./dicewin.lua"))
	// fmt.Printf("compileOut1: %s\n", compileOut1)
	// if compileErr1 != "" {
	// 	fmt.Println(compileErr1)
	// 	return
	// }

	// compileOut2, compileErr2 := execCommand(uvmCompilerPath, "-g", testContractPath("./dicewin_proxy.lua"))
	// fmt.Printf("compileOut2: %s\n", compileOut2)
	// if compileErr2 != "" {
	// 	fmt.Println(compileErr2)
	// 	return
	// }

	// 1
	res, err = simpleChainRPC("create_contract_from_file", caller1, testContractPath("./dicewin.gpc"), 50000, 10)
	assert.True(t, err == nil)
	contract1Addr := res.Get("contract_address").MustString()
	fmt.Printf("dicewin contract address: %s\n", contract1Addr)
	simpleChainRPC("generate_block")

	// 2
	res, err = simpleChainRPC("create_contract_from_file", caller1, testContractPath("./dicewin_proxy.gpc"), 50000, 10)
	assert.True(t, err == nil)
	contract2Addr := res.Get("contract_address").MustString()
	fmt.Printf("dicewin_proxy contract address: %s\n", contract2Addr)
	simpleChainRPC("generate_block")

	// 3
	res, err = simpleChainRPC("invoke_contract", caller1, contract2Addr, "set_dice_addr", []string{contract1Addr}, 0, 0, 50000, 10)
	assert.True(t, err == nil)
	transferTxid := res.Get("txid").MustString()
	println("set_dice_addr transfer txid: ", transferTxid)
	simpleChainRPC("generate_block")

	// 4
	res, err = invokeContractOffline(caller1, contract2Addr, "get_dice_addr", "")
	assert.True(t, err == nil)
	resJSONStr := res.Get("api_result").MustString()
	fmt.Printf("get_dice_addr: %s\n", resJSONStr)

	return
}

func testDicewinOffline(t *testing.T) {
	caller1 := "SPLtest1"
	var res *simplejson.Json
	var err error
	proxyContractAddr := "CONa2e4a1b2c2873c503c6ccbafe0be97cb16ed6d0e"
	userAddress := "HXNPL5kozBps7TN6zzdqGbTzFqFdXfkk8oGg"

	var upgrade int
	upgrade = 0
	if upgrade == 1 {
		res, err = simpleChainRPC("create_contract_from_file", caller1, testContractPath("./dicewin.gpc"), 50000, 10)
		assert.True(t, err == nil)
		contract1Addr := res.Get("contract_address").MustString()
		fmt.Printf("dicewin contract address: %s\n", contract1Addr)
		simpleChainRPC("generate_block")

		res, err = simpleChainRPC("invoke_contract", caller1, proxyContractAddr, "set_dice_addr", []string{contract1Addr}, 0, 0, 50000, 10)
		assert.True(t, err == nil)
		simpleChainRPC("generate_block")
	}

	var apiMap map[int]string
	apiMap = make(map[int]string)
	apiMap[1] = "query_dividend_config"
	apiMap[2] = "query_global_stat,HX"
	apiMap[3] = "query_global_hour_stat,HX,20190903,09"
	apiMap[4] = "query_global_today_hour_stat,HX"
	apiMap[5] = "query_user_stat," + userAddress + ",HX"
	apiMap[6] = "query_user_hour_stat," + userAddress + ",HX,20190903,09"
	apiMap[7] = "query_user_today_hour_stat," + userAddress + ",HX"
	apiMap[8] = "query_global_pawn_info,HX"
	apiMap[9] = "query_user_pawn_info," + userAddress + ",HX"
	apiMap[10] = "query_global_pawn_user_count,HX"
	apiMap[11] = "query_pawn_user_by_index,HX,1"
	apiMap[12] = "query_latest_dividend_time,HX"
	apiMap[13] = "query_next_dividend_time,HX"
	apiMap[14] = "query_user_dividend_info_estimate," + userAddress + ",HX"
	apiMap[15] = "query_dividend_time_by_index,HX,2"
	apiMap[16] = "query_global_ransom_list,HX,1,20"
	apiMap[17] = "query_user_dividend_result_by_day," + userAddress + ",20190904"
	apiMap[18] = "query_user_dividend_stat," + userAddress
	apiMap[19] = "query_global_dividend_info_estimate"
	apiMap[20] = "query_global_dividend_result_by_day,20190904"
	apiMap[21] = "query_global_dividend_info_list"
	apiMap[22] = "query_hd_liquidity"
	apiMap[23] = "query_mining_asset_info,HX"

	api_no := 22
	res, err = invokeContractOffline(caller1, proxyContractAddr, "call_api", apiMap[api_no])
	assert.True(t, err == nil)
	resJSONStr := res.Get("api_result").MustString()
	fmt.Printf("%s: %s\n", apiMap[api_no], resJSONStr)

	return
}

func testDicewinExec(t *testing.T) {
	caller1 := "SPLtest1"
	var res *simplejson.Json
	var err error

	proxyContractAddr := "CONa2e4a1b2c2873c503c6ccbafe0be97cb16ed6d0e"
	userAddress := "HXNPL5kozBps7TN6zzdqGbTzFqFdXfkk8oGg"

	upgrade := 0
	if upgrade == 1 {
		res, err = simpleChainRPC("create_contract_from_file", caller1, testContractPath("./dicewin.gpc"), 50000, 10)
		assert.True(t, err == nil)
		contract1Addr := res.Get("contract_address").MustString()
		fmt.Printf("dicewin contract address: %s\n", contract1Addr)
		simpleChainRPC("generate_block")

		res, err = simpleChainRPC("invoke_contract", caller1, proxyContractAddr, "set_dice_addr", []string{contract1Addr}, 0, 0, 50000, 10)
		assert.True(t, err == nil)
		simpleChainRPC("generate_block")
	}

	var apiMap map[int]string
	apiMap = make(map[int]string)
	apiMap[1] = "pawn," + userAddress + ",HX,10000"
	apiMap[2] = "ransom," + userAddress + ",HX,10000"
	apiMap[3] = "ransom_arrival,HX"
	apiMap[4] = "dividend"
	apiMap[5] = "set_dividend_switch_open,1"
	apiMap[6] = "set_dividend_profit_percent,50"
	apiMap[7] = "set_dividend_ransom_duration,3"
	apiMap[8] = "test_set_dividend"
	apiMap[9] = "test_gen_random_num,60,1"
	apiMap[10] = "play_bet,HX,100,50"
	apiMap[11] = "open"

	simpleChainRPC("generate_block")
	simpleChainRPC("generate_block")
	simpleChainRPC("generate_block")
	simpleChainRPC("generate_block")
	api_no := 11

	res, err = invokeContract(caller1, proxyContractAddr, "call_api", apiMap[api_no])
	assert.True(t, err == nil)
	resJSONStr := res.Get("api_result").MustString()
	fmt.Printf("%s: %s\n", apiMap[api_no], resJSONStr)
	simpleChainRPC("generate_block")

	return
}

// func TestDeployDicewin(t *testing.T) {
// 	fmt.Println("TestDicewin")
// 	cmd := execCommandBackground(simpleChainPath)
// 	assert.True(t, cmd != nil)
// 	fmt.Printf("simplechain pid: %d\n", cmd.Process.Pid)
// 	defer func() {
// 		kill(cmd)
// 	}()
// 	time.Sleep(1 * time.Second)
// 	testDeployDicewin(t)
// }

// func TestDicewinOffline(t *testing.T) {
// 	fmt.Println("TestDicewin")
// 	cmd := execCommandBackground(simpleChainPath)
// 	assert.True(t, cmd != nil)
// 	fmt.Printf("simplechain pid: %d\n", cmd.Process.Pid)
// 	defer func() {
// 		kill(cmd)
// 	}()
// 	time.Sleep(1 * time.Second)
// 	testDicewinOffline(t)
// }

func TestDicewinExec(t *testing.T) {
	fmt.Println("TestDicewin")
	cmd := execCommandBackground(simpleChainPath)
	assert.True(t, cmd != nil)
	fmt.Printf("simplechain pid: %d\n", cmd.Process.Pid)
	defer func() {
		kill(cmd)
	}()
	time.Sleep(1 * time.Second)
	testDicewinExec(t)
}

// ----------------------------------------------------
func invokeContractOffline(caller string, contractAddress string, apiName string, apiArg string) (*simplejson.Json, error) {
	res, err := simpleChainRPC("invoke_contract_offline", caller, contractAddress, apiName, []string{apiArg}, 0, 0)
	return res, err
}

func invokeContract(caller string, contractAddress string, apiName, apiArg string) (*simplejson.Json, error) {
	res, err := simpleChainRPC("invoke_contract", caller, contractAddress, apiName, []string{apiArg}, 0, 0, 100000, 100)
	return res, err
}
