#include "server.h"

static const error ERR_READ_ONLY = "You can't write against a read only replica";

static bool islstr(client *c, int arg_idx, const char *str){
	int i = 0;
	for (;i<c->args_size[arg_idx];i++){
		if (c->args[arg_idx][i] != str[i] && c->args[arg_idx][i] != str[i]-32){
			return false;
		}
	}
	return !str[i];
}

static bool iscmd(client *c, const char *cmd){
	return islstr(c, 0, cmd);
}

static void exec_set(client *c){
	const char **argv = c->args;
	int *argl = c->args_size;
	int argc = c->args_len;
	if (argc!=3){
		return client_write_error(c, "wrong number of arguments for 'set' command");
	}	
	if (readonly) {
		return client_write_error(c, ERR_READ_ONLY);
	}

	std::string key(argv[1], argl[1]);
	std::string value(argv[2], argl[2]);
	rocksdb::WriteOptions write_options;
	write_options.sync = !nosync;
	rocksdb::Status s = db->Put(write_options, key, value);
	if (!s.ok()){
		err(1, "%s", s.ToString().c_str());
	}
	client_write(c, "+OK\r\n", 5);
}

static void exec_mset(client *c){
	const char **argv = c->args+1;
	int *argl = c->args_size+1;
	int argc = c->args_len-1;
	if (argc<2 || argc%2 != 0){
		// Must set at least 1 key and each key must have a value.
		return client_write_error(c, "wrong number of arguments for 'mset' command");
	}
	if (readonly) {
		return client_write_error(c, ERR_READ_ONLY);
	}

	rocksdb::WriteBatch batch;
	for (int i = 0; i < argc; i += 2) {
		std::string key(argv[i], argl[i]);
		std::string value(argv[i+1], argl[i+1]);
		batch.Put(key, value);
	}

	rocksdb::WriteOptions write_options;
	write_options.sync = !nosync;
	rocksdb::Status s = db->Write(write_options, &batch);
	if (!s.ok()){
		err(1, "%s", s.ToString().c_str());
	}
	client_write(c, "+OK\r\n", 5);
}

static void exec_get(client *c){
	const char **argv = c->args;
	int *argl = c->args_size;
	int argc = c->args_len;
	if (argc!=2){
		return client_write_error(c, "wrong number of arguments for 'get' command");
	}	
	std::string key(argv[1], argl[1]);
	std::string value;
	rocksdb::Status s = db->Get(rocksdb::ReadOptions(), key, &value);
	if (!s.ok()){
		if (s.IsNotFound()){
			client_write(c, "$-1\r\n", 5);
			return;
		}
		err(1, "%s", s.ToString().c_str());
	}
	client_write_bulk(c, value.data(), value.size());
}

static void exec_mget(client *c){
	const char **argv = c->args+1;
	int *argl = c->args_size+1;
	int argc = c->args_len-1;
	if (argc<1){
		return client_write_error(c, "wrong number of arguments for 'mget' command");
	}

	std::vector<rocksdb::Slice> keys;
	std::vector<std::string> values;

	keys.reserve(argc);
	for (int i = 0; i < argc; ++i){
		keys.push_back(rocksdb::Slice(argv[i], argl[i]));
	}

	std::vector<rocksdb::Status> statuses = db->MultiGet(rocksdb::ReadOptions(), keys, &values);

	// It's possible only one of the keys failed, in which case the
	// whole operation should fail.
	for (int i = 0; i < argc; ++i) {
		const rocksdb::Status &s = statuses[i];
		if (s.ok() || s.IsNotFound())
			continue;
		err(1, "%s", s.ToString().c_str());
	}

	client_write_multibulk(c, argc);
	for (int i = 0; i < argc; ++i) {
		if (statuses[i].IsNotFound()){
			client_write(c, "$-1\r\n", 5);
		}else{
			const std::string &value = values[i];
			client_write_bulk(c, value.data(), value.size());
		}
	}
}

static void exec_del(client *c){
	const char **argv = c->args;
	int *argl = c->args_size;
	int argc = c->args_len;
	if (argc!=2){
		return client_write_error(c, "wrong number of arguments for 'del' command");
	}
	if (readonly) {
		return client_write_error(c, "You can't write against a read only replica");
	}
	std::string key(argv[1], argl[1]);
	std::string value; 
	rocksdb::Status s = db->Get(rocksdb::ReadOptions(), key, &value);
	if (!s.ok()){
		if (s.IsNotFound()){
			client_write(c, ":0\r\n", 4);
			return;
		}
		err(1, "%s", s.ToString().c_str());
	}
	rocksdb::WriteOptions write_options;
	write_options.sync = !nosync;
	s = db->Delete(write_options, key);
	if (!s.ok()){
		err(1, "%s", s.ToString().c_str());
	}
	client_write(c, ":1\r\n", 4);
}

static void exec_quit(client *c){
	client_write(c, "+OK\r\n", 5);
	err(1, "quit");
}

static void exec_flushdb(client *c){
	if (c->args_len!=1){
		return client_write_error(c, "wrong number of arguments for 'flushdb' command");
	}
	if (readonly) {
		return client_write_error(c, "You can't write against a read only replica");
	}
	flushdb();
	client_write(c, "+OK\r\n", 5);
}

static void exec_scan_keys(client *c,
		bool scan,
		const char *pat, int pat_len, 
		int cursor, int count
){
	if (count < 0){
		count = 10;
	}
	if (cursor < 0){
		cursor = 0;
	}

	char *start = NULL;
	char *end = NULL;
	int start_len = 0;
	int end_len = 0;
	int star = pattern_limits(pat, pat_len, &start, &start_len, &end, &end_len);
	std::string prefix(start, start_len);
	std::string postfix(end, end_len);
	
	// to avoid double-buffering, prewrite some bytes and then we'll go back 
	// and fill it in with correctness.
	const int filler = 128;
	for (int i=0;i<filler;i++){
		client_write_byte(c, '?');
	}
	int total = 0;
	int i = 0;
	int ncursor = 0;
	rocksdb::Iterator* it = db->NewIterator(rocksdb::ReadOptions());
	if (star){
		it->SeekToFirst();
	}else{
		it->Seek(prefix);
	}
	for (; it->Valid(); it->Next()) {
		rocksdb::Slice key = it->key();
		if (stringmatchlen(pat, pat_len, key.data(), key.size(), 1)){
			if (!star){
				int res = key.compare(postfix);
				if (res>=0){
					break;
				}
			}
			if (i >= cursor){
				if (scan&&total==count){
					ncursor = i;
					break;
				}
				client_write_bulk(c, key.data(), key.size());
				total++;	
			}
			i++;
		}
	}
	if (start){
		free(start);
	}
	if (end){
		free(end);
	}

	rocksdb::Status s = it->status();
	if (!s.ok()){
		err(1, "%s", s.ToString().c_str());	
	}
	delete it;

	// fill in the header and write from offset.
	char nb[filler];
	if (scan){
		char cursor_s[32];
		sprintf(cursor_s, "%d", ncursor);
		sprintf(nb, "*2\r\n$%zu\r\n%s\r\n*%d\r\n", strlen(cursor_s), cursor_s, total);
	}else{
		sprintf(nb, "*%d\r\n", total);
	}
	int nbn = strlen(nb);
	memcpy(c->output+filler-nbn, nb, nbn);
	c->output_offset = filler-nbn;
}

static void exec_keys(client *c){
	const char **argv = c->args;
	int *argl = c->args_size;
	int argc = c->args_len;
	if (argc!=2){
		return client_write_error(c, "wrong number of arguments for 'keys' command");
	}
	exec_scan_keys(c, false, argv[1], argl[1], -1, -1);
}

static void exec_scan(client *c){
	const char **argv = c->args;
	int *argl = c->args_size;
	int argc = c->args_len;
	if (argc<2){
		return client_write_error(c, "wrong number of arguments for 'scan' command");
	}
	int cursor = atop(argv[1], argl[1]);
	if (cursor < 0){
		return client_write_error(c, "invalid cursor");
	}
	int count = -1;
	const char *pat = "*";
	int pat_len = 1;

	for (int i=2;i<argc;i++){
		if (islstr(c, i, "match")){
			i++;
			if (i==argc){
				return client_write_error(c, "syntax error");
			}
			pat = argv[i];
			pat_len = argl[i];
		}else if (islstr(c, i, "count")){
			i++;
			if (i==argc){
				return client_write_error(c, "syntax error");
			}
			count = atop(argv[i], argl[i]);
			if (count < 0){
				return client_write_error(c, "value is not an integer or out of range");
			}
		}else{
			return client_write_error(c, "syntax error");
		}
	}
	return exec_scan_keys(c, true, pat, pat_len, cursor, count);
}


static const struct {
	const char *name;
	command_t command;
} commands[] = {
	{ "del",     &exec_del },
	{ "flushdb", &exec_flushdb },
	{ "get",     &exec_get },
	{ "mset",    &exec_mset },
	{ "mget",    &exec_mget },
	{ "quit",    &exec_quit },
	{ "scan",    &exec_scan },
	{ "set",     &exec_set },
};

void exec_command(client *c){
	command_t command = NULL;

	// TODO: use bsearch to find command
	for (size_t i = 0; i < sizeof(commands)/sizeof(*commands); i += 1){
		if (iscmd(c, commands[i].name)){
			command = commands[i].command;
			break;
		}
	}

	if (command){
		client_dispatch_command(c, command);
	}else{
		client_write_error(c, client_err_unknown_command(c, c->args[0], c->args_size[0]));
		client_flush(c);
	}
}

