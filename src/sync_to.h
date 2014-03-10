#include "command.h"
#include "sync_algorithm.h"
#include "schema_functions.h"
#include "sync_queue.h"
#include "table_row_applier.h"
#include "fdstream.h"
#include <boost/algorithm/string.hpp>
#include <thread>

using namespace std;

#define VERY_VERBOSE 2

template <typename DatabaseClient>
struct SyncToWorker {
	SyncToWorker(
		Database &database, SyncQueue &sync_queue, bool leader, int read_from_descriptor, int write_to_descriptor,
		const char *database_host, const char *database_port, const char *database_name, const char *database_username, const char *database_password,
		const set<string> &ignore_tables, const set<string> &only_tables, int verbose, bool snapshot, bool partial, bool rollback_after):
			database(database),
			sync_queue(sync_queue),
			leader(leader),
			input_stream(read_from_descriptor),
			output_stream(write_to_descriptor),
			input(input_stream),
			output(output_stream),
			client(database_host, database_port, database_name, database_username, database_password),
			ignore_tables(ignore_tables),
			only_tables(only_tables),
			verbose(verbose),
			snapshot(snapshot),
			rollback_after(rollback_after),
			partial(partial),
			protocol_version(0),
			worker_thread(std::ref(*this)) {
	}

	~SyncToWorker() {
		worker_thread.join();
	}

	void operator()() {
		try {
			negotiate_protocol();
			negotiate_target_block_size();
			share_snapshot();
			populate_database_schema();

			client.start_write_transaction();

			compare_schema();
			enqueue_tables();
			sync_tables();

			if (rollback_after) {
				rollback();
			} else {
				commit();
			}

			// send a quit so the other end closes its output and terminates gracefully
			send_quit_command();
		} catch (const exception &e) {
			// make sure all other workers terminate promptly, and if we are the first to fail, output the error
			if (sync_queue.abort()) {
				cerr << e.what() << endl;
			}

			// if the --partial option was used, try to commit the changes we've made, but ignore any errors,
			// and don't bother outputting timings
			if (partial) {
				try { client.commit_transaction(); } catch (...) {}
			}
		}

		// eagerly close the streams so that the SSH session terminates promptly on aborts
		output_stream.close();
	}

	void negotiate_protocol() {
		const int PROTOCOL_VERSION_SUPPORTED = 1;

		// tell the other end what version of the protocol we can speak, and have them tell us which version we're able to converse in
		send_command(output, Commands::PROTOCOL, PROTOCOL_VERSION_SUPPORTED);

		// read the response to the protocol_version command that the output thread sends when it starts
		// this is currently unused, but the command's semantics need to be in place for it to be useful in the future...
		input >> protocol_version;
	}

	void negotiate_target_block_size() {
		const size_t DEFAULT_MINIMUM_BLOCK_SIZE = 256*1024; // arbitrary, but needs to be big enough to cope with a moderate amount of latency

		send_command(output, Commands::TARGET_BLOCK_SIZE, DEFAULT_MINIMUM_BLOCK_SIZE);

		// the real app always accepts the block size we request, but the test suite uses smaller block sizes to make it easier to set up different scenarios
		input >> target_block_size;
	}

	void share_snapshot() {
		if (sync_queue.workers > 1 && snapshot) {
			// although some databases (such as postgresql) can share & adopt snapshots with no penalty
			// to other transactions, those that don't have an actual snapshot adoption mechanism (mysql)
			// need us to use blocking locks to prevent other transactions changing the data while they
			// start simultaneous transactions.  it's therefore important to minimize the time that we
			// hold the locks, so we wait for all workers to be up, running, and connected before
			// starting; this is also nicer (on all databases) in that it means no changes will be made
			// if some of the workers fail to start.
			sync_queue.wait_at_barrier();

			// now, request the lock or snapshot from the leader's peer.
			if (leader) {
				send_command(output, Commands::EXPORT_SNAPSHOT);
				sync_queue.snapshot = input.next<string>();
			}
			sync_queue.wait_at_barrier();

			// as soon as it has responded, adopt the snapshot/start the transaction in each of the other workers.
			if (!leader) {
				send_command(output, Commands::IMPORT_SNAPSHOT, sync_queue.snapshot);
				input.next_nil(); // arbitrary; sent by the other end once they've started their transaction
			}
			sync_queue.wait_at_barrier();

			// those databases that use locking instead of snapshot adoption can release the locks once
			// all the workers have started their transactions.
			if (leader) {
				send_command(output, Commands::UNHOLD_SNAPSHOT);
				input.next_nil(); // similarly arbitrary
			}
		} else {
			send_command(output, Commands::WITHOUT_SNAPSHOT);
			input.next_nil(); // similarly arbitrary
		}
	}

	void populate_database_schema() {
		if (leader) {
			client.populate_database_schema(database);
		}
	}

	void compare_schema() {
		// we could do this in all workers, but there's no need, and it'd waste a bit of traffic/time
		if (leader) {
			// get its schema
			send_command(output, Commands::SCHEMA);

			// read the response to the schema command that the output thread sends when it starts
			Database from_database;
			input >> from_database;

			// check they match
			check_schema_match(from_database, database, ignore_tables, only_tables);
		}
	}

	void enqueue_tables() {
		// queue up all the tables
		if (leader) {
			sync_queue.enqueue(database.tables, ignore_tables, only_tables);
		}

		// wait for the leader to do that (a barrier here is slightly excessive as we don't care if the other
		// workers are ready to start work, but it's not worth having another synchronisation mechanism for this)
		sync_queue.wait_at_barrier();
	}

	void sync_tables() {
		client.disable_referential_integrity();

		while (true) {
			// grab the next table to work on from the queue (blocking if it's empty)
			const Table *table = sync_queue.pop();

			// quit if there's no more tables to process
			if (!table) break;

			// synchronize that table (unfortunately we can't share this job with other workers because next-key
			// locking is used for unique key indexes to enforce the uniqueness constraint, so we can't share
			// write traffic to the database across connections, which makes it somewhat futile to try and farm the
			// read work out since that needs to see changes made to satisfy unique indexes earlier in the table)
			sync_table(*table);
		}

		// wait for all workers to finish their tables
		sync_queue.wait_at_barrier();
		client.enable_referential_integrity();
	}

	void sync_table(const Table &table) {
		TableRowApplier<DatabaseClient> row_applier(client, table);
		size_t hash_commands = 0;
		size_t rows_commands = 0;
		time_t started = time(nullptr);

		if (verbose) {
			unique_lock<mutex> lock(sync_queue.mutex);
			cout << "starting " << table.name << endl << flush;
		}

		send_command(output, Commands::OPEN, table.name);

		while (true) {
			sync_queue.check_aborted(); // check each iteration, rather than wait until the end of the current table; this is a good place to do it since it's likely we'll have no work to do for a short while

			Command command;
			input >> command;

			if (command.verb == Commands::HASH_NEXT) {
				// the last hash we sent them matched, and so they've moved on to the next set of rows and sent us the hash
				ColumnValues   prev_key(command.argument<ColumnValues>(0));
				ColumnValues   last_key(command.argument<ColumnValues>(1));
				string             hash(command.argument<string>(2));
				if (verbose >= VERY_VERBOSE) cout << "-> hash " << table.name << ' ' << non_binary_string_values_list(prev_key) << ' ' << non_binary_string_values_list(last_key) << endl;
				hash_commands++;

				// after each hash command received it's our turn to send the next command
				check_hash_and_choose_next_range(*this, table, nullptr, prev_key, last_key, nullptr, hash, target_block_size);

			} else if (command.verb == Commands::HASH_FAIL) {
				// the last hash we sent them didn't match, so they've reduced the key range and sent us back
				// the hash for a smaller set of rows (but not so small that they sent back the data instead)
				ColumnValues        prev_key(command.argument<ColumnValues>(0));
				ColumnValues        last_key(command.argument<ColumnValues>(1));
				ColumnValues failed_last_key(command.argument<ColumnValues>(2));
				string                  hash(command.argument<string>(3));
				if (verbose >= VERY_VERBOSE) cout << "-> hash " << table.name << ' ' << non_binary_string_values_list(prev_key) << ' ' << non_binary_string_values_list(last_key) << " last-failure " << non_binary_string_values_list(failed_last_key) << endl;
				hash_commands++;

				// after each hash command received it's our turn to send the next command
				check_hash_and_choose_next_range(*this, table, nullptr, prev_key, last_key, &failed_last_key, hash, target_block_size);

			} else if (command.verb == Commands::ROWS) {
				// we're being sent a range of rows; apply them to our end.  we do this in-context to
				// provide flow control - if we buffered and used a separate apply thread, we would
				// bloat up if this end couldn't write to disk as quickly as the other end sent data.
				ColumnValues prev_key(command.argument<ColumnValues>(0));
				ColumnValues last_key(command.argument<ColumnValues>(1));
				if (verbose >= VERY_VERBOSE) cout << "-> rows " << table.name << ' ' << non_binary_string_values_list(prev_key) << ' ' << non_binary_string_values_list(last_key) << endl;
				rows_commands++;

				row_applier.stream_from_input(input, prev_key, last_key);

				// if the range extends to the end of their table, that means we're done with this table;
				// otherwise, rows commands are immediately followed by another command
				if (last_key.empty()) break;
				
			} else if (command.verb == Commands::ROWS_AND_HASH_NEXT) {
				// combo of the above ROWS and HASH_NEXT commands
				ColumnValues prev_key(command.argument<ColumnValues>(0));
				ColumnValues last_key(command.argument<ColumnValues>(1));
				ColumnValues next_key(command.argument<ColumnValues>(2));
				string           hash(command.argument<string>(3));
				if (verbose >= VERY_VERBOSE) cout << "-> rows " << table.name << ' ' << non_binary_string_values_list(prev_key) << ' ' << non_binary_string_values_list(last_key) << " +" << endl;
				if (verbose >= VERY_VERBOSE) cout << "-> hash " << table.name << ' ' << non_binary_string_values_list(last_key) << ' ' << non_binary_string_values_list(next_key) << endl;
				hash_commands++;
				rows_commands++;

				// after each hash command received it's our turn to send the next command; we check
				// the hash and send the command *before* we stream in the rows that we're being sent
				// with this command as a simple form of pipelining - our next hash is going back
				// over the network at the same time as we are receiving rows.  we need to be able to
				// fit the command we send back in the kernel send buffer to guarantee there is no
				// deadlock; it's never been smaller than a page on any supported OS, and has been
				// defaulted to much larger values for some years.
				check_hash_and_choose_next_range(*this, table, nullptr, last_key, next_key, nullptr, hash, target_block_size);
				row_applier.stream_from_input(input, prev_key, last_key);
				// nb. it's implied last_key is not [], as we would have been sent back a plain rows command for the combined range if that was needed

			} else if (command.verb == Commands::ROWS_AND_HASH_FAIL) {
				// combo of the above ROWS and HASH_FAIL commands
				ColumnValues        prev_key(command.argument<ColumnValues>(0));
				ColumnValues        last_key(command.argument<ColumnValues>(1));
				ColumnValues        next_key(command.argument<ColumnValues>(2));
				ColumnValues failed_last_key(command.argument<ColumnValues>(3));
				string                  hash(command.argument<string>(4));
				if (verbose >= VERY_VERBOSE) cout << "-> rows " << table.name << ' ' << non_binary_string_values_list(prev_key) << ' ' << non_binary_string_values_list(last_key) << " +" << endl;
				if (verbose >= VERY_VERBOSE) cout << "-> hash " << table.name << ' ' << non_binary_string_values_list(last_key) << ' ' << non_binary_string_values_list(next_key) << " last-failure " << non_binary_string_values_list(failed_last_key) << endl;
				hash_commands++;
				rows_commands++;

				// same pipelining as the previous case
				check_hash_and_choose_next_range(*this, table, nullptr, last_key, next_key, &failed_last_key, hash, target_block_size);
				row_applier.stream_from_input(input, prev_key, last_key);

			} else {
				throw command_error("Unknown command " + to_string(command.verb));
			}
		}

		if (verbose) {
			time_t now = time(nullptr);
			unique_lock<mutex> lock(sync_queue.mutex);
			cout << "finished " << table.name << " in " << (now - started) << "s using " << hash_commands << " hash commands and " << rows_commands << " rows commands changing " << row_applier.rows_changed << " rows" << endl << flush;
		}
	}

	inline void send_hash_next_command(const Table &table, const ColumnValues &prev_key, const ColumnValues &last_key, const string &hash) {
		if (verbose >= VERY_VERBOSE) cout << "<- hash " << table.name << ' ' << non_binary_string_values_list(prev_key) << ' ' << non_binary_string_values_list(last_key) << endl;
		send_command(output, Commands::HASH_NEXT, prev_key, last_key, hash);
	}

	inline void send_hash_fail_command(const Table &table, const ColumnValues &prev_key, const ColumnValues &last_key, const ColumnValues &failed_last_key, const string &hash) {
		if (verbose >= VERY_VERBOSE) cout << "<- hash " << table.name << ' ' << non_binary_string_values_list(prev_key) << ' ' << non_binary_string_values_list(last_key) << " last-failure " << non_binary_string_values_list(failed_last_key) << endl;
		send_command(output, Commands::HASH_FAIL, prev_key, last_key, failed_last_key, hash);
	}

	inline void send_rows_command(const Table &table, const ColumnValues &prev_key, const ColumnValues &last_key) {
		if (verbose >= VERY_VERBOSE) cout << "<- rows " << table.name << ' ' << non_binary_string_values_list(prev_key) << ' ' << non_binary_string_values_list(last_key) << endl;
		send_command(output, Commands::ROWS, prev_key, last_key);
	}

	inline void send_rows_and_hash_next_command(const Table &table, const ColumnValues &prev_key, const ColumnValues &last_key, const ColumnValues &next_key, const string &hash) {
		if (verbose >= VERY_VERBOSE) cout << "<- rows " << table.name << ' ' << non_binary_string_values_list(prev_key) << ' ' << non_binary_string_values_list(last_key) << " +" << endl;
		if (verbose >= VERY_VERBOSE) cout << "<- hash " << table.name << ' ' << non_binary_string_values_list(last_key) << ' ' << non_binary_string_values_list(next_key) << endl;
		send_command(output, Commands::ROWS_AND_HASH_NEXT, prev_key, last_key, next_key, hash);
	}

	inline void send_rows_and_hash_fail_command(const Table &table, const ColumnValues &prev_key, const ColumnValues &last_key, const ColumnValues &next_key, const ColumnValues &failed_last_key, const string &hash) {
		if (verbose >= VERY_VERBOSE) cout << "<- rows " << table.name << ' ' << non_binary_string_values_list(prev_key) << ' ' << non_binary_string_values_list(last_key) << " +" << endl;
		if (verbose >= VERY_VERBOSE) cout << "<- hash " << table.name << ' ' << non_binary_string_values_list(prev_key) << ' ' << non_binary_string_values_list(last_key) << " last-failure " << non_binary_string_values_list(failed_last_key) << endl;
		send_command(output, Commands::ROWS_AND_HASH_FAIL, prev_key, last_key, next_key, failed_last_key, hash);
	}

	void commit() {
		time_t started = time(nullptr);

		client.commit_transaction();

		if (verbose) {
			time_t now = time(nullptr);
			unique_lock<mutex> lock(sync_queue.mutex);
			cout << "committed in " << (now - started) << "s" << endl << flush;
		}
	}

	void rollback() {
		time_t started = time(nullptr);

		client.rollback_transaction();

		if (verbose) {
			time_t now = time(nullptr);
			unique_lock<mutex> lock(sync_queue.mutex);
			cout << "rolled back in " << (now - started) << "s" << endl << flush;
		}
	}

	void send_quit_command() {
		try {
			send_command(output, Commands::QUIT);
		} catch (const exception &e) {
			// we don't care if sending this command fails itself, we're already past the point where we could abort anyway
		}
	}

	Database &database;
	SyncQueue &sync_queue;
	bool leader;
	FDWriteStream output_stream;
	FDReadStream input_stream;
	Unpacker<FDReadStream> input;
	Packer<FDWriteStream> output;
	DatabaseClient client;
	
	const set<string> ignore_tables;
	const set<string> only_tables;
	int verbose;
	bool snapshot;
	bool partial;
	bool rollback_after;

	int protocol_version;
	size_t target_block_size;
	std::thread worker_thread;
};

template <typename DatabaseClient, typename... Options>
void sync_to(int num_workers, int startfd, const Options &...options) {
	Database database;
	SyncQueue sync_queue(num_workers);
	vector<SyncToWorker<DatabaseClient>*> workers;

	workers.resize(num_workers);

	for (int worker = 0; worker < num_workers; worker++) {
		bool leader = (worker == 0);
		int read_from_descriptor = startfd + worker;
		int write_to_descriptor = startfd + worker + num_workers;
		workers[worker] = new SyncToWorker<DatabaseClient>(database, sync_queue, leader, read_from_descriptor, write_to_descriptor, options...);
	}

	for (typename vector<SyncToWorker<DatabaseClient>*>::const_iterator it = workers.begin(); it != workers.end(); ++it) delete *it;

	if (sync_queue.aborted) throw sync_error();
}
