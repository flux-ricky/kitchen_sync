require File.expand_path(File.join(File.dirname(__FILE__), 'test_helper'))

class HashFromTest < KitchenSync::EndpointTestCase
  include TestTableSchemas

  def from_or_to
    :from
  end

  def setup_with_footbl(**handshake_args)
    clear_schema
    create_footbl
    execute "INSERT INTO footbl VALUES (2, 10, 'test'), (4, NULL, 'foo'), (5, NULL, NULL), (8, -1, 'longer str'), (100, 0, 'last')"
    @rows = [[2,    10,       "test"],
             [4,   nil,        "foo"],
             [5,   nil,          nil],
             [8,    -1, "longer str"],
             [100,   0,       "last"]]
    @keys = @rows.collect {|row| [row[0]]}
    send_handshake_commands(**handshake_args)
  end

  test_each "calculates the hash of all the rows whose key is greater than the first argument and not greater than the last argument, and returns it and the row count" do
    setup_with_footbl

    send_command   Commands::HASH, ["footbl", @keys[0], @keys[1], 1000]
    expect_command Commands::HASH, ["footbl", @keys[0], @keys[1], 1000, 1, hash_of(@rows[1..1])]

    send_command   Commands::HASH, ["footbl", @keys[1], @keys[3], 1000]
    expect_command Commands::HASH, ["footbl", @keys[1], @keys[3], 1000, 2, hash_of(@rows[2..3])]
  end

  test_each "limits the number of rows within that range hashed to the given row count" do
    setup_with_footbl

    send_command   Commands::HASH, ["footbl", @keys[1], @keys[4], 3]
    expect_command Commands::HASH, ["footbl", @keys[1], @keys[4], 3, 3, hash_of(@rows[2..4])]

    send_command   Commands::HASH, ["footbl", @keys[1], @keys[4], 2]
    expect_command Commands::HASH, ["footbl", @keys[1], @keys[4], 2, 2, hash_of(@rows[2..3])]

    send_command   Commands::HASH, ["footbl", @keys[1], @keys[4], 1]
    expect_command Commands::HASH, ["footbl", @keys[1], @keys[4], 1, 1, hash_of(@rows[2..2])]
  end

  test_each "starts from the first row if an empty array is given as the first argument" do
    setup_with_footbl

    send_command   Commands::HASH, ["footbl", [], @keys[0], 1000]
    expect_command Commands::HASH, ["footbl", [], @keys[0], 1000, 1, hash_of(@rows[0..0])]

    send_command   Commands::HASH, ["footbl", [], @keys[1], 1000]
    expect_command Commands::HASH, ["footbl", [], @keys[1], 1000, 2, hash_of(@rows[0..1])]

    send_command   Commands::HASH, ["footbl", [], @keys[1], 1]
    expect_command Commands::HASH, ["footbl", [], @keys[1], 1, 1, hash_of(@rows[0..0])]
  end

  test_each "supports composite keys" do
    clear_schema
    create_secondtbl
    execute "INSERT INTO secondtbl VALUES (2, 2349174, 'xy', 1), (9, 968116383, 'aa', 9), (100, 100, 'aa', 100), (340, 363401169, 'ab', 20)"
    @rows = [[100,       100, "aa", 100], # first because the second column is the first term in the key so it's sorted like ["aa", 100]
             [  9, 968116383, "aa",   9],
             [340, 363401169, "ab",  20],
             [  2,   2349174, "xy",   1]]
    # note that the primary key columns are in reverse order to the table definition; the command arguments need to be given in the key order, but the column order for the results is unrelated
    @keys = @rows.collect {|row| [row[2], row[1]]}
    send_handshake_commands

    send_command   Commands::HASH, ["secondtbl",       [], @keys[0], 1000]
    expect_command Commands::HASH, ["secondtbl",       [], @keys[0], 1000, 1, hash_of(@rows[0..0])]

    send_command   Commands::HASH, ["secondtbl", @keys[0], @keys[2], 1000]
    expect_command Commands::HASH, ["secondtbl", @keys[0], @keys[2], 1000, 2, hash_of(@rows[1..2])]

    send_command   Commands::HASH, ["secondtbl", ["aa", "101"], @keys[1], 1000]
    expect_command Commands::HASH, ["secondtbl", ["aa", "101"], @keys[1], 1000, 1, hash_of(@rows[1..1])]

    send_command   Commands::HASH, ["secondtbl", @keys[1], @keys[3], 1000]
    expect_command Commands::HASH, ["secondtbl", @keys[1], @keys[3], 1000, 2, hash_of(@rows[2..3])]

    send_command   Commands::HASH, ["secondtbl",       [], ["aa", "101"], 1000]
    expect_command Commands::HASH, ["secondtbl",       [], ["aa", "101"], 1000, 1, hash_of(@rows[0..0])]

    send_command   Commands::HASH, ["secondtbl", ["aa", "101"], @keys[2], 1000]
    expect_command Commands::HASH, ["secondtbl", ["aa", "101"], @keys[2], 1000, 2, hash_of(@rows[1..2])]

    send_command   Commands::HASH, ["secondtbl", @keys[1], [], 1000]
    expect_command Commands::HASH, ["secondtbl", @keys[1], [], 1000, 2, hash_of(@rows[2..3])]
  end

  test_each "uses the chosen substitute key if the table has no real primary key but has a suitable unique key" do
    clear_schema
    create_noprimarytbl(create_suitable_keys: true)
    execute "INSERT INTO noprimarytbl (nullable, version, name, non_nullable) VALUES (2, 'a2349174', 'xy', 1), (NULL, 'b968116383', 'aa', 9)"
    @rows = [[2,     "a2349174", 'xy', 1],
             [nil, "b968116383", 'aa', 9]]
    @keys = @rows.collect {|row| [row[1]]}
    send_handshake_commands

    send_command   Commands::HASH, ["noprimarytbl",       [], @keys[0], 1000]
    expect_command Commands::HASH, ["noprimarytbl",       [], @keys[0], 1000, 1, hash_of(@rows[0..0])]

    send_command   Commands::HASH, ["noprimarytbl", @keys[0], @keys[1], 1000]
    expect_command Commands::HASH, ["noprimarytbl", @keys[0], @keys[1], 1000, 1, hash_of(@rows[1..1])]
  end

  test_each "skips auto-generated columns" do
    omit "Database doesn't support auto-generated columns" unless connection.supports_generated_columns?
    clear_schema
    create_generatedtbl
    execute "INSERT INTO generatedtbl (pri, fore, back) VALUES (1, 10, 100), (2, 20, 200)"
    @rows = [[1, 10, 100],
             [2, 20, 200]]
    @keys = @rows.collect {|row| [row[0]]}
    send_handshake_commands

    send_command   Commands::HASH, ["generatedtbl", [], [], 1000]
    expect_command Commands::HASH, ["generatedtbl", [], [], 1000, 2, hash_of(@rows)]
  end

  test_each "uses the chosen column order and adds a row count if the table has no real primary key or suitable unique key but has only non-nullable columns and a useful index" do
    clear_schema
    create_noprimaryjointbl(create_keys: true)
    execute "INSERT INTO noprimaryjointbl (table1_id, table2_id) VALUES (1, 100), (1, 101), (2, 101), (3, 9), (3, 10), (3, 10), (3, 11)"
    @rows = [[3, 9, 1], # sorted earlier than the rows with lower table1_id as the (table2_id, table1_d) index will get used
             [3, 10, 2],
             [3, 11, 1],
             [1, 100, 1],
             [1, 101, 1],
             [2, 101, 1]]
    @keys = @rows.collect {|row| [row[1], row[0]]}
    send_handshake_commands

    send_command   Commands::HASH, ["noprimaryjointbl",       [], @keys[0], 1000]
    expect_command Commands::HASH, ["noprimaryjointbl",       [], @keys[0], 1000, 1, hash_of(@rows[0..0])]

    send_command   Commands::HASH, ["noprimaryjointbl",       [], @keys[1], 1000]
    expect_command Commands::HASH, ["noprimaryjointbl",       [], @keys[1], 1000, 2, hash_of(@rows[0..1])] # note row count is 2 when in fact there were 3 underlying database rows

    send_command   Commands::HASH, ["noprimaryjointbl",       [], @keys[-1], 2]
    expect_command Commands::HASH, ["noprimaryjointbl",       [], @keys[-1], 2, 2, hash_of(@rows[0..1])] # note includes the third input row even though the row limit was set to 2

    send_command   Commands::HASH, ["noprimaryjointbl",       [], @keys[-1], 3]
    expect_command Commands::HASH, ["noprimaryjointbl",       [], @keys[-1], 3, 3, hash_of(@rows[0..2])]

    send_command   Commands::HASH, ["noprimaryjointbl", @keys[0], @keys[1], 1000]
    expect_command Commands::HASH, ["noprimaryjointbl", @keys[0], @keys[1], 1000, 1, hash_of(@rows[1..1])]
  end

  test_each "optionally supports xxHash64 hashes" do
    setup_with_footbl(target_minimum_block_size: 1, hash_algorithm: HashAlgorithm::XXH64)

    send_command   Commands::HASH, ["footbl", @keys[1], @keys[3], 1000]
    expect_command Commands::HASH, ["footbl", @keys[1], @keys[3], 1000, 2, hash_of(@rows[2..3], HashAlgorithm::XXH64)]

    send_command   Commands::HASH, ["footbl", [], @keys[1], 1000]
    expect_command Commands::HASH, ["footbl", [], @keys[1], 1000, 2, hash_of(@rows[0..1], HashAlgorithm::XXH64)]

    send_command   Commands::HASH, ["footbl", [], @keys[1], 1]
    expect_command Commands::HASH, ["footbl", [], @keys[1], 1, 1, hash_of(@rows[0..0], HashAlgorithm::XXH64)]
  end

  test_each "optionally supports MD5 hashes" do
    setup_with_footbl(target_minimum_block_size: 1, hash_algorithm: HashAlgorithm::MD5)

    send_command   Commands::HASH, ["footbl", @keys[1], @keys[3], 1000]
    expect_command Commands::HASH, ["footbl", @keys[1], @keys[3], 1000, 2, hash_of(@rows[2..3], HashAlgorithm::MD5)]

    send_command   Commands::HASH, ["footbl", [], @keys[1], 1000]
    expect_command Commands::HASH, ["footbl", [], @keys[1], 1000, 2, hash_of(@rows[0..1], HashAlgorithm::MD5)]

    send_command   Commands::HASH, ["footbl", [], @keys[1], 1]
    expect_command Commands::HASH, ["footbl", [], @keys[1], 1, 1, hash_of(@rows[0..0], HashAlgorithm::MD5)]
  end
end
