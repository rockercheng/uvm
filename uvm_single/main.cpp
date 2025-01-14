#include <uvm/lprefix.h>

#include <iostream>
#include <fstream>
#include <memory>
#include <string>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uvm/lua.h>
#include "uvm_api.demo.h"

#include <uvm/lauxlib.h>
#include <uvm/lualib.h>
#include <uvm/uvm_lib.h>
#include <boost/filesystem/path.hpp>
#include <boost/algorithm/hex.hpp>
#include <fc/crypto/hex.hpp>


#if !defined(LUA_PROMPT)
#define LUA_PROMPT		"> "
#define LUA_PROMPT2		">> "
#endif

#if !defined(LUA_PROGNAME)
#define LUA_PROGNAME		"uvm"
#endif

#if !defined(LUA_MAXINPUT)
#define LUA_MAXINPUT		512
#endif

#if !defined(LUA_INIT_VAR)
#define LUA_INIT_VAR		"LUA_INIT"
#endif

#define LUA_INITVARVERSION  \
	LUA_INIT_VAR "_" LUA_VERSION_MAJOR "_" LUA_VERSION_MINOR


static lua_State *globalL = NULL;

static const char *progname = LUA_PROGNAME;


/*
** Hook set by signal function to stop the interpreter.
*/
static void lstop(lua_State *L, lua_Debug *ar) {
	(void)ar;  /* unused arg. */
	lua_sethook(L, nullptr, 0, 0);  /* reset hook */
	luaL_error(L, "interrupted!");
}


/*
** Function to be called at a C signal. Because a C signal cannot
** just change a uvm state (as there is no proper synchronization),
** this function only sets a hook that, when called, will stop the
** interpreter.
*/
static void laction(int i) {
	signal(i, SIG_DFL); /* if another SIGINT happens, terminate process */
	lua_sethook(globalL, lstop, LUA_MASKCALL | LUA_MASKRET | LUA_MASKCOUNT, 1);
}


static void print_usage(const char *badoption) {
	lua_writestringerror("%s: ", progname);
	if (badoption[1] == 'e' || badoption[1] == 'l')
		lua_writestringerror("'%s' needs argument\n", badoption);
	else
		lua_writestringerror("unrecognized option '%s'\n", badoption);
	lua_writestringerror(
		"usage: %s [options] [script [args]]\n"
		"Available options are:\n"
		"  -e stat  execute string 'stat'\n"
		"  -i       enter interactive mode after executing 'script'\n"
		"  -l name  require library 'name'\n"
		"  -v       show version information\n"
		"  -E       ignore environment variables\n"
		"  -d       decompile bytecode to source\n"
		"  -s       disassemble bytecode to readable assemble\n"
		"  -r       run bytecode file\n"
		"  -t       run contract testcases, load script_path + '.test' bytecode file(contains a function accept contract table) to run testcases\n"
		"  -k       call contract api, -k script_path contract_api api_argument [caller_address caller_pubkey]\n"
		"  -x       run with debugger\n"
		"  -c       compile source to bytecode\n"
		"  -h       show help info\n"
		"  --       stop handling options\n"
		"  -        stop handling options and execute stdin\n"
		,
		progname);
}


/*
** Prints an error message, adding the program name in front of it
** (if present)
*/
static void l_message(const char *pname, const char *msg) {
	if (pname) lua_writestringerror("%s: ", pname);
	lua_writestringerror("%s\n", msg);
}


/*
** Check whether 'status' is not OK and, if so, prints the error
** message on the top of the stack. It assumes that the error object
** is a string, as it was either generated by uvm or by 'msghandler'.
*/
static int report(lua_State *L, int status) {
	if (status != LUA_OK) {
		const char *msg = lua_tostring(L, -1);
		l_message(progname, msg);
		lua_pop(L, 1);  /* remove message */
	}
	return status;
}


/*
** Message handler used to run all chunks
*/
static int msghandler(lua_State *L) {
	const char *msg = lua_tostring(L, 1);
	if (msg == NULL) {  /* is error object not a string? */
		if (luaL_callmeta(L, 1, "__tostring") &&  /* does it have a metamethod */
			lua_type(L, -1) == LUA_TSTRING)  /* that produces a string? */
			return 1;  /* that is the message */
		else
			msg = lua_pushfstring(L, "(error object is a %s value)",
				luaL_typename(L, 1));
	}
	luaL_traceback(L, L, msg, 1);  /* append a standard traceback */
	return 1;  /* return the traceback */
}


/*
** Interface to 'lua_pcall', which sets appropriate message function
** and C-signal handler. Used to run all chunks.
*/
static int docall(lua_State *L, int narg, int nres) {
	int status;
	int base = lua_gettop(L) - narg;  /* function index */
	lua_pushcfunction(L, msghandler);  /* push message handler */
	lua_insert(L, base);  /* put it under function and args */
	globalL = L;  /* to be available to 'laction' */
	signal(SIGINT, laction);  /* set C-signal handler */
	status = lua_pcall(L, narg, nres, base);
	signal(SIGINT, SIG_DFL); /* reset C-signal handler */
	lua_remove(L, base);  /* remove message handler from the stack */
	return status;
}


static void print_version(void) {
	lua_writestring(LUA_COPYRIGHT, strlen(LUA_COPYRIGHT));
	lua_writeline();
}

/*
** Create the 'arg' table, which stores all arguments from the
** command line ('argv'). It should be aligned so that, at index 0,
** it has 'argv[script]', which is the script name. The arguments
** to the script (everything after 'script') go to positive indices;
** other arguments (before the script name) go to negative indices.
** If there is no script name, assume interpreter's name as base.
*/
static void createargtable(lua_State *L, char **argv, int argc, int script) {
	int i, narg;
	if (script == argc) script = 0;  /* no script name? */
	narg = argc - (script + 1);  /* number of positive indices */
	lua_createtable(L, narg, script + 1);
	for (i = 0; i < argc; i++) {
		lua_pushstring(L, argv[i]);
		lua_rawseti(L, -2, i - script);
	}
	lua_setglobal(L, "arg");
}


static int dochunk(lua_State *L, int status) {
	if (status == LUA_OK) status = docall(L, 0, 0);
	return report(L, status);
}


static int dofile(lua_State *L, const char *name) {
	return dochunk(L, luaL_loadfile(L, name));
}

static int dostring(lua_State *L, const char *s, const char *name) {
	return dochunk(L, luaL_loadbuffer(L, s, strlen(s), name));
}

/*
** lua_readline defines how to show a prompt and then read a line from
** the standard input.
** lua_saveline defines how to "save" a read line in a "history".
** lua_freeline defines how to free a line read by lua_readline.
*/
#if !defined(lua_readline)	/* { */

#if defined(LUA_USE_READLINE)	/* { */

#include <readline/readline.h>
#include <readline/history.h>
#define lua_readline(L,b,p)	((void)L, ((b)=readline(p)) != NULL)
#define lua_saveline(L,line)	((void)L, add_history(line))
#define lua_freeline(L,b)	((void)L, free(b))

#else				/* }{ */

#define lua_readline(L,b,p) \
        ((void)L, fputs(p, stdout), fflush(stdout),  /* show prompt */ \
        fgets(b, LUA_MAXINPUT, stdin) != NULL)  /* get line */
#define lua_saveline(L,line)	{ (void)L; (void)line; }
#define lua_freeline(L,b)	{ (void)L; (void)b; }

#endif				/* } */

#endif				/* } */



/*
** Calls 'require(name)' and stores the result in a global variable
** with the given name.
*/
//static int dolibrary(lua_State *L, const char *name) {
//	int status;
//	lua_getglobal(L, "require");
//	lua_pushstring(L, name);
//	status = docall(L, 1, 1);  /* call 'require(name)' */
//	if (status == LUA_OK)
//		lua_setglobal(L, name);  /* global[name] = require return */
//	return report(L, status);
//}

/*
** Returns the string to be used as a prompt by the interpreter.
*/
//static const char *get_prompt(lua_State *L, int firstline) {
//	const char *p;
//	lua_getglobal(L, firstline ? "_PROMPT" : "_PROMPT2");
//	p = lua_tostring(L, -1);
//	if (p == NULL) p = (firstline ? LUA_PROMPT : LUA_PROMPT2);
//	return p;
//}

/* mark in error messages for incomplete statements */
#define EOFMARK		"<eof>"
#define marklen		(sizeof(EOFMARK)/sizeof(char) - 1)

static std::string uvm_pending_source; // 未完成输入的uvm源码
static std::string lua_pending_source; // 未完成输入的lua源码



/*
** Push on the stack the contents of table 'arg' from 1 to #arg
*/
static int pushargs(lua_State *L) {
	int i, n;
	if (lua_getglobal(L, "arg") != LUA_TTABLE)
		luaL_error(L, "'arg' is not a table");
	n = (int)luaL_len(L, -1);
	luaL_checkstack(L, n + 3, "too many arguments to script");
	for (i = 1; i <= n; i++)
		lua_rawgeti(L, -i, i);
	lua_remove(L, -i);  /* remove table from the stack */
	return n;
}

//static bool use_type_check_compile = true;

static int handle_script(lua_State *L, char **argv, bool is_contract = false) {
	int status;
	const char *fname = argv[0];
	if (strcmp(fname, "-") == 0 && strcmp(argv[-1], "--") != 0)
		fname = NULL;  /* stdin */
	bool is_bytecode_file = luaL_is_bytecode_file(L, fname);
	if (is_bytecode_file)
	{
		status = luaL_loadfile(L, fname);
		if (status == LUA_OK) {
			int n = pushargs(L);  /* push arguments to script */
			status = docall(L, n, LUA_MULTRET);
		}
		return report(L, status);
	}
	else
	{
		auto error = "not bytecode file";
		perror(error);
		lua_set_compile_error(L, error);
		return LUA_ERRRUN;
	}
}


/* bits of various argument indicators in 'args' */
#define has_error	1	/* bad option */
#define has_v		4	/* -v */
#define has_e		8	/* -e */
#define has_E		16	/* -E */
#define has_run     32  /* -r */
#define has_test    64  /* -t */
#define has_call    128 /* -k */
#define has_debug   256 /* -x */
#define has_help    512 /* -h */

/*
** Traverses all arguments from 'argv', returning a mask with those
** needed before running any uvm code (or an error code if it finds
** any invalid argument). 'first' returns the first not-handled argument
** (either the script name or a bad argument in case of error).
*/
static int collectargs(char **argv, int *first) {
	int args = 0;
	int i;
	for (i = 1; argv[i] != NULL; i++) {
		*first = i;
		if (argv[i][0] != '-')  /* not an option? */
			return args;  /* stop handling options */
		switch (argv[i][1]) {  /* else check option */
		case '-':  /* '--' */
			if (argv[i][2] != '\0')  /* extra characters after '--'? */
				return has_error;  /* invalid option */
			*first = i + 1;
			return args;
		case '\0':  /* '-' */
			return args;  /* script "name" is '-' */
		case 'E':
			if (argv[i][2] != '\0')  /* extra characters after 1st? */
				return has_error;  /* invalid option */
			args |= has_E;
			break;
		case 'v':
			if (argv[i][2] != '\0')  /* extra characters after 1st? */
				return has_error;  /* invalid option */
			args |= has_v;
			break;
		case 'r':
			if (argv[i][2] != '\0')  /* extra characters after 1st? */
				return has_error;  /* invalid option */
			args |= has_run;
			break;
		case 't':
			if (argv[i][2] != '\0')  /* extra characters after 1st? */
				return has_error;  /* invalid option */
			args |= has_test;
			break;
		case 'k':
			if (argv[i][2] != '\0')  /* extra characters after 1st? */
				return has_error;  /* invalid option */
			args |= has_call;
			break;
		case 'x':
			if (argv[i][2] != '\0')  /* extra characters after 1st? */
				return has_error;  /* invalid option */
			args |= has_debug;
			break;
		case 'h':
			if (argv[i][2] != '\0')  /* extra characters after 1st? */
				return has_error;  /* invalid option */
			args |= has_help;
			break;
		case 'e':
			args |= has_e;  /* FALLTHROUGH */
		case 'l':  /* both options need an argument */
			if (argv[i][2] == '\0') {  /* no concatenated argument? */
				i++;  /* try next 'argv' */
				if (argv[i] == NULL || argv[i][0] == '-')
					return has_error;  /* no next argument or it is another option */
			}
			break;
		default:  /* invalid option */
			return has_error;
		}
	}
	*first = i;  /* no script name */
	return args;
}

static int handle_luainit(lua_State *L) {
	const char *name = "=" LUA_INITVARVERSION;
	const char *init = getenv(name + 1);
	if (init == NULL) {
		name = "=" LUA_INIT_VAR;
		init = getenv(name + 1);  /* try alternative name */
	}
	if (init == NULL) return LUA_OK;
	else if (init[0] == '@')
		return dofile(L, init + 1);
	else
		return dostring(L, init, name);
}

/*
** Main body of stand-alone interpreter (to be called in protected mode).
** Reads the options and handles them all.
*/
static int pmain(lua_State *L) {
	int argc = (int)lua_tointeger(L, 1);
	char **argv = (char **)lua_touserdata(L, 2);
	int script;
	int args = collectargs(argv, &script);
	luaL_checkversion(L);  /* check that interpreter has correct version */
	if (argv[0] && argv[0][0]) progname = argv[0];
	if (args == has_error) {  /* bad arg? */
		print_usage(argv[script]);  /* 'script' has index of bad arg. */
		return LUA_OK;
	}
	if (args & has_help) {
		print_usage("help");
		return LUA_OK;
	}
	if (args & has_v)  /* option '-v'? */
		print_version();
	if (args & has_E) {  /* option '-E'? */
		lua_pushboolean(L, 1);  /* signal for libraries to ignore env. vars. */
		lua_setfield(L, LUA_REGISTRYINDEX, "LUA_NOENV");
	}
	luaL_openlibs(L);  /* open standard libraries */
	createargtable(L, argv, argc, script);  /* create table 'arg' */
	if (!(args & has_E)) {  /* no option '-E'? */
		if (handle_luainit(L) != LUA_OK)  /* run LUA_INIT */
			return 0;  /* error running LUA_INIT */
	}
	if ((args & has_call) && (script >= argc - 2)) {
		perror("-k need pass contract api and api argument after script path");
		return 0;
	}
	std::string contract_api;
	std::string contract_api_arg;
	std::string caller_address = "";
	std::string caller_pubkey = "";
	if (args & has_call) {
		contract_api = argv[script + 1];
		contract_api_arg = argv[script + 2];
		if (argc > script + 3) {
			caller_address = argv[script + 3];
		}
		if (argc > script + 4) {
			caller_pubkey = argv[script + 4];

		}
	}

	//if (!runargs(L, argv, script))  /* execute arguments -e and -l */
	//	return 0;  /* something failed */
	if (script < argc) {
		auto run_script_result = handle_script(L, argv + script); /* execute main script (if there is one) */
		if (run_script_result != LUA_OK) {
			return LUA_ERRERR;
		}
		if (args & has_call) {
			// call contract api
			std::string result_string;
			cbor::CborArrayValue arr;
			arr.push_back(cbor::CborObject::from_string(contract_api_arg));
			if (!uvm::lua::lib::call_last_contract_api(L, std::string(argv[script]), contract_api, arr,caller_address,caller_pubkey, &result_string)) {
				return LUA_ERRERR;
			}
			printf("result: %s\n", result_string.c_str());
			return LUA_OK;
		}
		if (args & has_test) {
			// run ***.test, whose content is a function accept a contract table
			lua_setglobal(L, "_test_contract");
			auto test_script_path = std::string(argv[script]) + ".test";
			if (script < argc - 1) {
				test_script_path = argv[script + 1];
			}
			char *path = (char*) malloc(test_script_path.size() + 1);
			memcpy(path, test_script_path.c_str(), test_script_path.size());
			path[test_script_path.size()] = '\0';
			char* paths[1];
			paths[0] = path;
			auto load_test_script_result = handle_script(L, paths);
			if (load_test_script_result != LUA_OK) {
				return 1;
			}
			if (!lua_isfunction(L, -1)) {
				perror("test script must contains a function accept contract table");
				return 1;
			}
			lua_getglobal(L, "_test_contract");
			auto result = lua_pcall(L, 1, 1, 0);
			if (result != LUA_OK) {
				return 1;
			}
			printf("test done\n");
			return 0;
		}
		if (args & has_debug) {
			// TODO
		}
	}
	if (script == argc && !(args & (has_e | has_v))) {  /* no arguments? */
															 /*
															 if (lua_stdin_is_tty()) {  // running in interactive mode?
															 print_version();
															 doREPL(L);  // do read-eval-print loop
															 }
															 */
															 // else 
															 // dofile(L, NULL);  /* executes stdin as a file */
		printf("need filename arg");
	}
	lua_pushboolean(L, 1);  /* signal no errors */
	return 1;
}

using uvm::lua::api::global_uvm_chain_api;

int main(int argc, char **argv) {
	global_uvm_chain_api = new uvm::lua::api::DemoUvmChainApi();
	int status, result;
	uvm::lua::lib::UvmStateScope scope(true, true);
	scope.add_system_extra_libs();
	lua_State *L = scope.L();
	if (!L) {
		l_message(argv[0], "cannot create state: not enough memory");
		return EXIT_FAILURE;
	}
	try {
		lua_pushcfunction(L, &pmain);  /* to call 'pmain' in protected mode */
		lua_pushinteger(L, argc);  /* 1st argument */
		lua_pushlightuserdata(L, argv); /* 2nd argument */
		status = lua_pcall(L, 2, 1, 0);  /* do the call */
		result = lua_toboolean(L, -1);  /* get result */
		report(L, status);
		return (result && status == LUA_OK) ? EXIT_SUCCESS : EXIT_FAILURE;
	}
	catch (fc::exception &e) {
		printf(e.to_string().c_str());
		return EXIT_FAILURE;
	}
	catch (std::exception &e) {
		printf(e.what());
		return EXIT_FAILURE;
	}
	catch (...) {
		printf("unknow exception");
		return EXIT_FAILURE;
	}
	
}
