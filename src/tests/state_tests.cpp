/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gmock/gmock.h>

#include <list>
#include <set>
#include <string>
#include <vector>

#include <mesos/mesos.hpp>

#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/protobuf.hpp>
#include <process/pid.hpp>

#include <stout/gtest.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include <stout/protobuf.hpp>

#include "common/type_utils.hpp"

#include "log/log.hpp"
#include "log/replica.hpp"

#include "log/tool/initialize.hpp"

#include "master/registry.hpp"

#include "messages/state.hpp"

#include "state/in_memory.hpp"
#include "state/leveldb.hpp"
#include "state/log.hpp"
#include "state/protobuf.hpp"
#include "state/storage.hpp"
#include "state/zookeeper.hpp"

#include "tests/utils.hpp"
#ifdef MESOS_HAS_JAVA
#include "tests/zookeeper.hpp"
#endif

using namespace mesos;
using namespace mesos::log;

using namespace process;

using mesos::state::InMemoryStorage;
using mesos::state::LevelDBStorage;
using mesos::state::LogStorage;
using mesos::state::Operation;
using mesos::state::Storage;
#ifdef MESOS_HAS_JAVA
using mesos::state::ZooKeeperStorage;
#endif

using mesos::state::protobuf::State;
using mesos::state::protobuf::Variable;

using std::list;
using std::set;
using std::string;
using std::vector;

using mesos::tests::TemporaryDirectoryTest;

typedef mesos::Registry::Slaves Slaves;
typedef mesos::Registry::Slave Slave;

void FetchAndStoreAndFetch(State* state)
{
  Future<Variable<Slaves> > future1 = state->fetch<Slaves>("slaves");
  AWAIT_READY(future1);

  Variable<Slaves> variable = future1.get();

  Slaves slaves1 = variable.get();
  ASSERT_EQ(0, slaves1.slaves().size());

  Slave* slave = slaves1.add_slaves();
  slave->mutable_info()->set_hostname("localhost");

  variable = variable.mutate(slaves1);

  Future<Option<Variable<Slaves> > > future2 = state->store(variable);
  AWAIT_READY(future2);
  ASSERT_SOME(future2.get());

  future1 = state->fetch<Slaves>("slaves");
  AWAIT_READY(future1);

  variable = future1.get();

  Slaves slaves2 = variable.get();
  ASSERT_EQ(1, slaves2.slaves().size());
  EXPECT_EQ("localhost", slaves2.slaves(0).info().hostname());
}


void FetchAndStoreAndStoreAndFetch(State* state)
{
  Future<Variable<Slaves> > future1 = state->fetch<Slaves>("slaves");
  AWAIT_READY(future1);

  Variable<Slaves> variable = future1.get();

  Slaves slaves1 = variable.get();
  ASSERT_EQ(0, slaves1.slaves().size());

  Slave* slave = slaves1.add_slaves();
  slave->mutable_info()->set_hostname("localhost");

  variable = variable.mutate(slaves1);

  Future<Option<Variable<Slaves> > > future2 = state->store(variable);
  AWAIT_READY(future2);
  ASSERT_SOME(future2.get());

  variable = future2.get().get();

  future2 = state->store(variable);
  AWAIT_READY(future2);
  ASSERT_SOME(future2.get());

  future1 = state->fetch<Slaves>("slaves");
  AWAIT_READY(future1);

  variable = future1.get();

  Slaves slaves2 = variable.get();
  ASSERT_EQ(1, slaves2.slaves().size());
  EXPECT_EQ("localhost", slaves2.slaves(0).info().hostname());
}


void FetchAndStoreAndStoreFailAndFetch(State* state)
{
  Future<Variable<Slaves> > future1 = state->fetch<Slaves>("slaves");
  AWAIT_READY(future1);

  Variable<Slaves> variable1 = future1.get();

  Slaves slaves1 = variable1.get();
  ASSERT_EQ(0, slaves1.slaves().size());

  Slave* slave1 = slaves1.add_slaves();
  slave1->mutable_info()->set_hostname("localhost1");

  Variable<Slaves> variable2 = variable1.mutate(slaves1);

  Future<Option<Variable<Slaves> > > future2 = state->store(variable2);
  AWAIT_READY(future2);
  ASSERT_SOME(future2.get());

  Slaves slaves2 = variable1.get();
  ASSERT_EQ(0, slaves2.slaves().size());

  Slave* slave2 = slaves2.add_slaves();
  slave2->mutable_info()->set_hostname("localhost2");

  variable2 = variable1.mutate(slaves2);

  future2 = state->store(variable2);
  AWAIT_READY(future2);
  EXPECT_TRUE(future2.get().isNone());

  future1 = state->fetch<Slaves>("slaves");
  AWAIT_READY(future1);

  variable1 = future1.get();

  slaves1 = variable1.get();
  ASSERT_EQ(1, slaves1.slaves().size());
  EXPECT_EQ("localhost1", slaves1.slaves(0).info().hostname());
}


void FetchAndStoreAndExpungeAndFetch(State* state)
{
  Future<Variable<Slaves> > future1 = state->fetch<Slaves>("slaves");
  AWAIT_READY(future1);

  Variable<Slaves> variable = future1.get();

  Slaves slaves1 = variable.get();
  ASSERT_EQ(0, slaves1.slaves().size());

  Slave* slave = slaves1.add_slaves();
  slave->mutable_info()->set_hostname("localhost");

  variable = variable.mutate(slaves1);

  Future<Option<Variable<Slaves> > > future2 = state->store(variable);
  AWAIT_READY(future2);
  ASSERT_SOME(future2.get());

  variable = future2.get().get();

  Future<bool> future3 = state->expunge(variable);
  AWAIT_READY(future3);
  ASSERT_TRUE(future3.get());

  future1 = state->fetch<Slaves>("slaves");
  AWAIT_READY(future1);

  variable = future1.get();

  Slaves slaves2 = variable.get();
  ASSERT_EQ(0, slaves2.slaves().size());
}


void FetchAndStoreAndExpungeAndExpunge(State* state)
{
  Future<Variable<Slaves> > future1 = state->fetch<Slaves>("slaves");
  AWAIT_READY(future1);

  Variable<Slaves> variable = future1.get();

  Slaves slaves1 = variable.get();
  ASSERT_EQ(0, slaves1.slaves().size());

  Slave* slave = slaves1.add_slaves();
  slave->mutable_info()->set_hostname("localhost");

  variable = variable.mutate(slaves1);

  Future<Option<Variable<Slaves> > > future2 = state->store(variable);
  AWAIT_READY(future2);
  ASSERT_SOME(future2.get());

  variable = future2.get().get();

  Future<bool> future3 = state->expunge(variable);
  AWAIT_READY(future3);
  ASSERT_TRUE(future3.get());

  future3 = state->expunge(variable);
  AWAIT_READY(future3);
  ASSERT_FALSE(future3.get());
}


void FetchAndStoreAndExpungeAndStoreAndFetch(State* state)
{
  Future<Variable<Slaves> > future1 = state->fetch<Slaves>("slaves");
  AWAIT_READY(future1);

  Variable<Slaves> variable = future1.get();

  Slaves slaves1 = variable.get();
  ASSERT_EQ(0, slaves1.slaves().size());

  Slave* slave = slaves1.add_slaves();
  slave->mutable_info()->set_hostname("localhost");

  variable = variable.mutate(slaves1);

  Future<Option<Variable<Slaves> > > future2 = state->store(variable);
  AWAIT_READY(future2);
  ASSERT_SOME(future2.get());

  variable = future2.get().get();

  Future<bool> future3 = state->expunge(variable);
  AWAIT_READY(future3);
  ASSERT_TRUE(future3.get());

  future2 = state->store(variable);
  AWAIT_READY(future2);
  ASSERT_SOME(future2.get());

  future1 = state->fetch<Slaves>("slaves");
  AWAIT_READY(future1);

  variable = future1.get();

  Slaves slaves2 = variable.get();
  ASSERT_EQ(1, slaves2.slaves().size());
  EXPECT_EQ("localhost", slaves2.slaves(0).info().hostname());
}


void Names(State* state)
{
  Future<Variable<Slaves> > future1 = state->fetch<Slaves>("slaves");
  AWAIT_READY(future1);

  Variable<Slaves> variable = future1.get();

  Slaves slaves1 = variable.get();
  ASSERT_EQ(0, slaves1.slaves().size());

  Slave* slave = slaves1.add_slaves();
  slave->mutable_info()->set_hostname("localhost");

  variable = variable.mutate(slaves1);

  Future<Option<Variable<Slaves> > > future2 = state->store(variable);
  AWAIT_READY(future2);
  ASSERT_SOME(future2.get());

  Future<set<string> > names = state->names();
  AWAIT_READY(names);
  ASSERT_EQ(1u, names.get().size());
  EXPECT_NE(names.get().find("slaves"), names.get().end());
}


class InMemoryStateTest : public ::testing::Test
{
public:
  InMemoryStateTest()
    : storage(NULL),
      state(NULL) {}

protected:
  virtual void SetUp()
  {
    storage = new InMemoryStorage();
    state = new State(storage);
  }

  virtual void TearDown()
  {
    delete state;
    delete storage;
  }

  Storage* storage;
  State* state;
};


TEST_F(InMemoryStateTest, FetchAndStoreAndFetch)
{
  FetchAndStoreAndFetch(state);
}


TEST_F(InMemoryStateTest, FetchAndStoreAndStoreAndFetch)
{
  FetchAndStoreAndStoreAndFetch(state);
}


TEST_F(InMemoryStateTest, FetchAndStoreAndStoreFailAndFetch)
{
  FetchAndStoreAndStoreFailAndFetch(state);
}


TEST_F(InMemoryStateTest, FetchAndStoreAndExpungeAndFetch)
{
  FetchAndStoreAndExpungeAndFetch(state);
}


TEST_F(InMemoryStateTest, FetchAndStoreAndExpungeAndExpunge)
{
  FetchAndStoreAndExpungeAndExpunge(state);
}


TEST_F(InMemoryStateTest, FetchAndStoreAndExpungeAndStoreAndFetch)
{
  FetchAndStoreAndExpungeAndStoreAndFetch(state);
}


TEST_F(InMemoryStateTest, Names)
{
  Names(state);
}


class LevelDBStateTest : public ::testing::Test
{
public:
  LevelDBStateTest()
    : storage(NULL),
      state(NULL),
      path(os::getcwd() + "/.state") {}

protected:
  virtual void SetUp()
  {
    os::rmdir(path);
    storage = new LevelDBStorage(path);
    state = new State(storage);
  }

  virtual void TearDown()
  {
    delete state;
    delete storage;
    os::rmdir(path);
  }

  Storage* storage;
  State* state;

private:
  const string path;
};


TEST_F(LevelDBStateTest, FetchAndStoreAndFetch)
{
  FetchAndStoreAndFetch(state);
}


TEST_F(LevelDBStateTest, FetchAndStoreAndStoreAndFetch)
{
  FetchAndStoreAndStoreAndFetch(state);
}


TEST_F(LevelDBStateTest, FetchAndStoreAndStoreFailAndFetch)
{
  FetchAndStoreAndStoreFailAndFetch(state);
}


TEST_F(LevelDBStateTest, FetchAndStoreAndExpungeAndFetch)
{
  FetchAndStoreAndExpungeAndFetch(state);
}


TEST_F(LevelDBStateTest, FetchAndStoreAndExpungeAndExpunge)
{
  FetchAndStoreAndExpungeAndExpunge(state);
}


TEST_F(LevelDBStateTest, FetchAndStoreAndExpungeAndStoreAndFetch)
{
  FetchAndStoreAndExpungeAndStoreAndFetch(state);
}


TEST_F(LevelDBStateTest, Names)
{
  Names(state);
}


class LogStateTest : public TemporaryDirectoryTest
{
public:
  LogStateTest()
    : storage(NULL),
      state(NULL),
      replica2(NULL),
      log(NULL) {}

protected:
  virtual void SetUp()
  {
    TemporaryDirectoryTest::SetUp();

    // For initializing the replicas.
    tool::Initialize initializer;

    string path1 = os::getcwd() + "/.log1";
    string path2 = os::getcwd() + "/.log2";

    initializer.flags.path = path1;
    initializer.execute();

    initializer.flags.path = path2;
    initializer.execute();

    // Only create the replica for 'path2' (i.e., the second replica)
    // as the first replica will be created when we create a Log.
    replica2 = new Replica(path2);

    set<UPID> pids;
    pids.insert(replica2->pid());

    log = new Log(2, path1, pids);
    storage = new LogStorage(log, 1024);
    state = new State(storage);
  }

  virtual void TearDown()
  {
    delete state;
    delete storage;
    delete log;

    delete replica2;

    TemporaryDirectoryTest::TearDown();
  }

  Storage* storage;
  State* state;

  Replica* replica2;
  Log* log;
};


TEST_F(LogStateTest, FetchAndStoreAndFetch)
{
  FetchAndStoreAndFetch(state);
}


TEST_F(LogStateTest, FetchAndStoreAndStoreAndFetch)
{
  FetchAndStoreAndStoreAndFetch(state);
}


TEST_F(LogStateTest, FetchAndStoreAndStoreFailAndFetch)
{
  FetchAndStoreAndStoreFailAndFetch(state);
}


TEST_F(LogStateTest, FetchAndStoreAndExpungeAndFetch)
{
  FetchAndStoreAndExpungeAndFetch(state);
}


TEST_F(LogStateTest, FetchAndStoreAndExpungeAndExpunge)
{
  FetchAndStoreAndExpungeAndExpunge(state);
}


TEST_F(LogStateTest, FetchAndStoreAndExpungeAndStoreAndFetch)
{
  FetchAndStoreAndExpungeAndStoreAndFetch(state);
}


TEST_F(LogStateTest, Names)
{
  Names(state);
}


Future<Option<Variable<Slaves> > > timeout(
    Future<Option<Variable<Slaves> > > future)
{
  future.discard();
  return Failure("Timeout");
}


TEST_F(LogStateTest, Timeout)
{
  Clock::pause();

  Future<Variable<Slaves> > future1 = state->fetch<Slaves>("slaves");
  AWAIT_READY(future1);

  Variable<Slaves> variable = future1.get();

  Slaves slaves1 = variable.get();
  ASSERT_EQ(0, slaves1.slaves().size());

  Slave* slave = slaves1.add_slaves();
  slave->mutable_info()->set_hostname("localhost");

  variable = variable.mutate(slaves1);

  // Now terminate the replica so the store will timeout.
  terminate(replica2->pid());
  wait(replica2->pid());

  Future<Option<Variable<Slaves> > > future2 = state->store(variable);

  Future<Option<Variable<Slaves> > > future3 =
    future2.after(Seconds(5), lambda::bind(&timeout, lambda::_1));

  ASSERT_TRUE(future2.isPending());
  ASSERT_TRUE(future3.isPending());

  Clock::advance(Seconds(5));

  AWAIT_DISCARDED(future2);
  AWAIT_FAILED(future3);

  Clock::resume();
}


TEST_F(LogStateTest, Diff)
{
  Future<Variable<Slaves>> future1 = state->fetch<Slaves>("slaves");
  AWAIT_READY(future1);

  Variable<Slaves> variable = future1.get();

  Slaves slaves = variable.get();
  ASSERT_EQ(0, slaves.slaves().size());

  for (size_t i = 0; i < 1024; i++) {
    Slave* slave = slaves.add_slaves();
    slave->mutable_info()->set_hostname("localhost" + stringify(i));
  }

  variable = variable.mutate(slaves);

  Future<Option<Variable<Slaves>>> future2 = state->store(variable);
  AWAIT_READY(future2);
  ASSERT_SOME(future2.get());

  variable = future2.get().get();

  Slave* slave = slaves.add_slaves();
  slave->mutable_info()->set_hostname("localhost1024");

  variable = variable.mutate(slaves);

  future2 = state->store(variable);
  AWAIT_READY(future2);
  ASSERT_SOME(future2.get());

  // It's possible that we're doing truncation asynchronously which
  // will cause the test to fail because we'll end up getting a
  // pending position from Log::Reader::ending which will cause
  // Log::Reader::read to fail. To remedy this, we pause the clock and
  // wait for all executing processe to settle.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  Log::Reader reader(log);

  Future<Log::Position> beginning = reader.beginning();
  Future<Log::Position> ending = reader.ending();

  AWAIT_READY(beginning);
  AWAIT_READY(ending);

  Future<list<Log::Entry>> entries = reader.read(beginning.get(), ending.get());

  AWAIT_READY(entries);

  // Convert each Log::Entry to a Operation.
  vector<Operation> operations;

  foreach (const Log::Entry& entry, entries.get()) {
    // Parse the Operation from the Log::Entry.
    Operation operation;

    google::protobuf::io::ArrayInputStream stream(
        entry.data.data(),
        entry.data.size());

    ASSERT_TRUE(operation.ParseFromZeroCopyStream(&stream));

    operations.push_back(operation);
  }

  ASSERT_EQ(2u, operations.size());
  EXPECT_EQ(Operation::SNAPSHOT, operations[0].type());
  EXPECT_EQ(Operation::DIFF, operations[1].type());
}


#ifdef MESOS_HAS_JAVA
class ZooKeeperStateTest : public tests::ZooKeeperTest
{
public:
  ZooKeeperStateTest()
    : storage(NULL),
      state(NULL) {}

protected:
  virtual void SetUp()
  {
    ZooKeeperTest::SetUp();
    storage = new ZooKeeperStorage(
        server->connectString(),
        NO_TIMEOUT,
        "/state/");
    state = new State(storage);
  }

  virtual void TearDown()
  {
    delete state;
    delete storage;
    ZooKeeperTest::TearDown();
  }

  Storage* storage;
  State* state;
};


TEST_F(ZooKeeperStateTest, FetchAndStoreAndFetch)
{
  FetchAndStoreAndFetch(state);
}


TEST_F(ZooKeeperStateTest, FetchAndStoreAndStoreAndFetch)
{
  FetchAndStoreAndStoreAndFetch(state);
}


TEST_F(ZooKeeperStateTest, FetchAndStoreAndStoreFailAndFetch)
{
  FetchAndStoreAndStoreFailAndFetch(state);
}


TEST_F(ZooKeeperStateTest, FetchAndStoreAndExpungeAndFetch)
{
  FetchAndStoreAndExpungeAndFetch(state);
}


TEST_F(ZooKeeperStateTest, FetchAndStoreAndExpungeAndExpunge)
{
  FetchAndStoreAndExpungeAndExpunge(state);
}


TEST_F(ZooKeeperStateTest, FetchAndStoreAndExpungeAndStoreAndFetch)
{
  FetchAndStoreAndExpungeAndStoreAndFetch(state);
}


TEST_F(ZooKeeperStateTest, Names)
{
  Names(state);
}
#endif // MESOS_HAS_JAVA
