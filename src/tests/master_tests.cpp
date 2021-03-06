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

#include <unistd.h>

#include <gmock/gmock.h>

#include <string>
#include <vector>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include <process/metrics/counter.hpp>
#include <process/metrics/metrics.hpp>

#include <stout/json.hpp>
#include <stout/net.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include "master/allocator.hpp"
#include "master/flags.hpp"
#include "master/master.hpp"

#include "slave/constants.hpp"
#include "slave/gc.hpp"
#include "slave/flags.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"

using namespace mesos;
using namespace mesos::tests;

using mesos::master::Master;

using mesos::master::allocator::AllocatorProcess;
using mesos::master::allocator::HierarchicalDRFAllocatorProcess;

using mesos::slave::GarbageCollectorProcess;
using mesos::slave::Slave;
using mesos::slave::Containerizer;
using mesos::slave::MesosContainerizerProcess;

using process::Clock;
using process::Future;
using process::Owned;
using process::PID;
using process::UPID;

using std::string;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::DoAll;
using testing::Eq;
using testing::Return;
using testing::SaveArg;

// Those of the overall Mesos master/slave/scheduler/driver tests
// that seem vaguely more master than slave-related are in this file.
// The others are in "slave_tests.cpp".

class MasterTest : public MesosTest {};


TEST_F(MasterTest, TaskRunning)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  Try<PID<Slave> > slave = StartSlave(&containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<Nothing> update;
  EXPECT_CALL(containerizer,
              update(_, Resources(offers.get()[0].resources())))
    .WillOnce(DoAll(FutureSatisfy(&update),
                    Return(Future<Nothing>())));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  AWAIT_READY(update);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// This test ensures that stopping a scheduler driver triggers
// executor's shutdown callback and all still running tasks are
// marked as killed.
TEST_F(MasterTest, ShutdownFrameworkWhileTaskRunning)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  slave::Flags flags = CreateSlaveFlags();
  flags.executor_shutdown_grace_period = Seconds(0);

  Try<PID<Slave>> slave = StartSlave(&containerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());
  Offer offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offer.slave_id());
  task.mutable_resources()->MergeFrom(offer.resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<Nothing> update;
  EXPECT_CALL(containerizer,
              update(_, Resources(offer.resources())))
    .WillOnce(DoAll(FutureSatisfy(&update),
                    Return(Future<Nothing>())));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offer.id(), tasks);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  AWAIT_READY(update);

  // Set expectation that Master receives UnregisterFrameworkMessage,
  // which triggers marking running tasks as killed.
  UnregisterFrameworkMessage message;
  message.mutable_framework_id()->MergeFrom(offer.framework_id());

  Future<UnregisterFrameworkMessage> unregisterFrameworkMessage =
    FUTURE_PROTOBUF(message, _, master.get());

  // Set expectation that Executor's shutdown callback is invoked.
  Future<Nothing> shutdown;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(FutureSatisfy(&shutdown));

  // Stop the driver while the task is running.
  driver.stop();
  driver.join();

  // Wait for UnregisterFrameworkMessage message to be dispatched and
  // executor's shutdown callback to be called.
  AWAIT_READY(unregisterFrameworkMessage);
  AWAIT_READY(shutdown);

  // We have to be sure the UnregisterFrameworkMessage is processed
  // completely and running tasks enter a terminal state before we
  // request the master state.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  // Request master state.
  Future<process::http::Response> response =
    process::http::get(master.get(), "state.json");
  AWAIT_READY(response);

  // These checks are not essential for the test, but may help
  // understand what went wrong.
  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(parse);

  // Make sure the task landed in completed and marked as killed.
  Result<JSON::String> state = parse.get().find<JSON::String>(
      "completed_frameworks[0].completed_tasks[0].state");

  ASSERT_SOME_EQ(JSON::String("TASK_KILLED"), state);

  Shutdown();  // Must shutdown before 'containerizer' gets deallocated.
}


TEST_F(MasterTest, KillTask)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave> > slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskID taskId;
  taskId.set_value("1");

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->MergeFrom(taskId);
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  EXPECT_CALL(exec, killTask(_, _))
    .WillOnce(SendStatusUpdateFromTaskID(TASK_KILLED));

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.killTask(taskId);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_KILLED, status.get().state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown();
}


// This test ensures that a killTask for an unknown task results in a
// TASK_LOST when there are no slaves in transitionary states.
TEST_F(MasterTest, KillUnknownTask)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave> > slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskID taskId;
  taskId.set_value("1");

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->MergeFrom(taskId);
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  TaskID unknownTaskId;
  unknownTaskId.set_value("2");

  driver.killTask(unknownTaskId);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_LOST, status.get().state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown();
}


TEST_F(MasterTest, KillUnknownTaskSlaveInTransition)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get());

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  // Start a checkpointing slave.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.checkpoint = true;

  Try<PID<Slave> > slave = StartSlave(&exec, slaveFlags);
  ASSERT_SOME(slave);

  // Wait for slave registration.
  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID slaveId = slaveRegisteredMessage.get().slave_id();

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &detector);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  // Start a task.
  TaskInfo task = createTask(offers.get()[0], "", DEFAULT_EXECUTOR_ID);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  Future<Nothing> _reregisterSlave =
    DROP_DISPATCH(_, &Master::_reregisterSlave);

  // Stop master and slave.
  Stop(master.get());
  Stop(slave.get());

  frameworkId = Future<FrameworkID>();
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  // Restart master.
  master = StartMaster();
  ASSERT_SOME(master);

  Future<Nothing> disconnected;
  EXPECT_CALL(sched, disconnected(&driver))
    .WillOnce(FutureSatisfy(&disconnected));

  // Simulate a spurious event (e.g., due to ZooKeeper
  // expiration) at the scheduler.
  detector.appoint(master.get());

  AWAIT_READY(frameworkId);

  // Restart slave.
  slave = StartSlave(&exec, slaveFlags);

  // Wait for the slave to start reregistration.
  AWAIT_READY(_reregisterSlave);

  // As Master::killTask isn't doing anything, we shouldn't get a status update.
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .Times(0);

  // Set expectation that Master receives killTask message.
  Future<KillTaskMessage> killTaskMessage =
    FUTURE_PROTOBUF(KillTaskMessage(), _, master.get());

  // Attempt to kill unknown task while slave is transitioning.
  TaskID unknownTaskId;
  unknownTaskId.set_value("2");

  ASSERT_FALSE(unknownTaskId == task.task_id());

  Clock::pause();

  driver.killTask(unknownTaskId);

  AWAIT_READY(killTaskMessage);

  // Wait for all messages to be dispatched and processed completely to satisfy
  // the expectation that we didn't receive a status update.
  Clock::settle();

  Clock::resume();

  driver.stop();
  driver.join();

  Shutdown();
}


TEST_F(MasterTest, StatusUpdateAck)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave> > slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<StatusUpdateAcknowledgementMessage> acknowledgement =
    FUTURE_PROTOBUF(StatusUpdateAcknowledgementMessage(), _, Eq(slave.get()));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  // Ensure the slave gets a status update ACK.
  AWAIT_READY(acknowledgement);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown();
}


TEST_F(MasterTest, RecoverResources)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = Option<string>(
      "cpus:2;mem:1024;disk:1024;ports:[1-10, 20-30]");

  Try<PID<Slave> > slave = StartSlave(&containerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  ExecutorInfo executorInfo;
  executorInfo.MergeFrom(DEFAULT_EXECUTOR_INFO);

  Resources executorResources =
    Resources::parse("cpus:0.3;mem:200;ports:[5-8, 23-25]").get();
  executorInfo.mutable_resources()->MergeFrom(executorResources);

  TaskID taskId;
  taskId.set_value("1");

  Resources taskResources = offers.get()[0].resources() - executorResources;

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->MergeFrom(taskId);
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(taskResources);
  task.mutable_executor()->MergeFrom(executorInfo);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  EXPECT_CALL(exec, killTask(_, _))
    .WillOnce(SendStatusUpdateFromTaskID(TASK_KILLED));

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  // Scheduler should get an offer for killed task's resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.killTask(taskId);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_KILLED, status.get().state());

  driver.reviveOffers(); // Don't wait till the next allocation.

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  Offer offer = offers.get()[0];
  EXPECT_EQ(taskResources, offer.resources());

  driver.declineOffer(offer.id());

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  // Now kill the executor, scheduler should get an offer it's resources.
  containerizer.destroy(offer.framework_id(), executorInfo.executor_id());

  // TODO(benh): We can't do driver.reviveOffers() because we need to
  // wait for the killed executors resources to get aggregated! We
  // should wait for the allocator to recover the resources first. See
  // the allocator tests for inspiration.

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());
  Resources slaveResources = Resources::parse(flags.resources.get()).get();
  EXPECT_EQ(slaveResources, offers.get()[0].resources());

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


TEST_F(MasterTest, FrameworkMessage)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave> > slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver schedDriver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&schedDriver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&schedDriver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  schedDriver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<ExecutorDriver*> execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(FutureArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&schedDriver, _))
    .WillOnce(FutureArg<1>(&status));

  schedDriver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  Future<string> execData;
  EXPECT_CALL(exec, frameworkMessage(_, _))
    .WillOnce(FutureArg<1>(&execData));

  schedDriver.sendFrameworkMessage(
      DEFAULT_EXECUTOR_ID, offers.get()[0].slave_id(), "hello");

  AWAIT_READY(execData);
  EXPECT_EQ("hello", execData.get());

  Future<string> schedData;
  EXPECT_CALL(sched, frameworkMessage(&schedDriver, _, _, _))
    .WillOnce(FutureArg<3>(&schedData));

  execDriver.get()->sendFrameworkMessage("world");

  AWAIT_READY(schedData);
  EXPECT_EQ("world", schedData.get());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  schedDriver.stop();
  schedDriver.join();

  Shutdown();
}


TEST_F(MasterTest, MultipleExecutors)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  ExecutorInfo executor1; // Bug in gcc 4.1.*, must assign on next line.
  executor1 = CREATE_EXECUTOR_INFO("executor-1", "exit 1");

  ExecutorInfo executor2; // Bug in gcc 4.1.*, must assign on next line.
  executor2 = CREATE_EXECUTOR_INFO("executor-2", "exit 1");

  MockExecutor exec1(executor1.executor_id());
  MockExecutor exec2(executor2.executor_id());

  hashmap<ExecutorID, Executor*> execs;
  execs[executor1.executor_id()] = &exec1;
  execs[executor2.executor_id()] = &exec2;

  TestContainerizer containerizer(execs);

  Try<PID<Slave> > slave = StartSlave(&containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  TaskInfo task1;
  task1.set_name("");
  task1.mutable_task_id()->set_value("1");
  task1.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task1.mutable_resources()->MergeFrom(
      Resources::parse("cpus:1;mem:512").get());
  task1.mutable_executor()->MergeFrom(executor1);

  TaskInfo task2;
  task2.set_name("");
  task2.mutable_task_id()->set_value("2");
  task2.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task2.mutable_resources()->MergeFrom(
      Resources::parse("cpus:1;mem:512").get());
  task2.mutable_executor()->MergeFrom(executor2);

  vector<TaskInfo> tasks;
  tasks.push_back(task1);
  tasks.push_back(task2);

  EXPECT_CALL(exec1, registered(_, _, _, _))
    .Times(1);

  Future<TaskInfo> exec1Task;
  EXPECT_CALL(exec1, launchTask(_, _))
    .WillOnce(DoAll(SendStatusUpdateFromTask(TASK_RUNNING),
                    FutureArg<1>(&exec1Task)));

  EXPECT_CALL(exec2, registered(_, _, _, _))
    .Times(1);

  Future<TaskInfo> exec2Task;
  EXPECT_CALL(exec2, launchTask(_, _))
    .WillOnce(DoAll(SendStatusUpdateFromTask(TASK_RUNNING),
                    FutureArg<1>(&exec2Task)));

  Future<TaskStatus> status1, status2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(exec1Task);
  EXPECT_EQ(task1.task_id(), exec1Task.get().task_id());

  AWAIT_READY(exec2Task);
  EXPECT_EQ(task2.task_id(), exec2Task.get().task_id());

  AWAIT_READY(status1);
  EXPECT_EQ(TASK_RUNNING, status1.get().state());

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_RUNNING, status2.get().state());

  EXPECT_CALL(exec1, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(exec2, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


TEST_F(MasterTest, MasterInfo)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  Try<PID<Slave> > slave = StartSlave();
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<MasterInfo> masterInfo;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<2>(&masterInfo));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return()); // Ignore offers.

  driver.start();

  AWAIT_READY(masterInfo);
  EXPECT_EQ(master.get().node.port, masterInfo.get().port());
  EXPECT_EQ(master.get().node.ip, masterInfo.get().ip());

  driver.stop();
  driver.join();

  Shutdown();
}


TEST_F(MasterTest, MasterInfoOnReElection)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get());

  Try<PID<Slave> > slave = StartSlave(&detector);
  ASSERT_SOME(slave);

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &detector);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureSatisfy(&resourceOffers));

  Future<process::Message> message =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  driver.start();

  AWAIT_READY(message);
  AWAIT_READY(resourceOffers);

  Future<Nothing> disconnected;
  EXPECT_CALL(sched, disconnected(&driver))
    .WillOnce(FutureSatisfy(&disconnected));

  Future<MasterInfo> masterInfo;
  EXPECT_CALL(sched, reregistered(&driver, _))
    .WillOnce(FutureArg<1>(&masterInfo));

  Future<Nothing> resourceOffers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureSatisfy(&resourceOffers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Simulate a spurious event (e.g., due to ZooKeeper
  // expiration) at the scheduler.
  detector.appoint(master.get());

  AWAIT_READY(disconnected);

  AWAIT_READY(masterInfo);
  EXPECT_EQ(master.get().node.port, masterInfo.get().port());
  EXPECT_EQ(master.get().node.ip, masterInfo.get().ip());

  // The re-registered framework should get offers.
  AWAIT_READY(resourceOffers2);

  driver.stop();
  driver.join();

  Shutdown();
}


class WhitelistTest : public MasterTest
{
protected:
  WhitelistTest()
    : path("whitelist.txt")
  {}

  virtual ~WhitelistTest()
  {
    os::rm(path);
  }
  const string path;
};


TEST_F(WhitelistTest, WhitelistSlave)
{
  // Add some hosts to the white list.
  Try<string> hostname = net::hostname();
  ASSERT_SOME(hostname);

  string hosts = hostname.get() + "\n" + "dummy-slave";
  ASSERT_SOME(os::write(path, hosts)) << "Error writing whitelist";

  master::Flags flags = CreateMasterFlags();
  flags.whitelist = "file://" + path;

  Try<PID<Master> > master = StartMaster(flags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.hostname = hostname.get();
  Try<PID<Slave> > slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers); // Implies the slave has registered.

  driver.stop();
  driver.join();

  Shutdown();
}


TEST_F(MasterTest, MasterLost)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get());

  Try<PID<Slave> > slave = StartSlave();
  ASSERT_SOME(slave);

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &detector);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return()); // Ignore offers.

  Future<process::Message> message =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  driver.start();

  AWAIT_READY(message);

  Future<Nothing> disconnected;
  EXPECT_CALL(sched, disconnected(&driver))
    .WillOnce(FutureSatisfy(&disconnected));

  // Simulate a spurious event at the scheduler.
  detector.appoint(None());

  AWAIT_READY(disconnected);

  driver.stop();
  driver.join();

  Shutdown();
}


// Test ensures two offers from same slave can be used for single task.
// This is done by first launching single task which utilize half of the
// available resources. A subsequent offer for the rest of the available
// resources will be sent by master. The first task is killed and an offer
// for the remaining resources will be sent. Which means two offers covering
// all slave resources and a single task should be able to run on these.
TEST_F(MasterTest, LaunchCombinedOfferTest)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  // The CPU granularity is 1.0 which means that we need slaves with at least
  // 2 cpus for a combined offer.
  Resources halfSlave = Resources::parse("cpus:1;mem:512").get();
  Resources fullSlave = halfSlave + halfSlave;

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = Option<string>(stringify(fullSlave));

  Try<PID<Slave> > slave = StartSlave(&containerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // Get 1st offer and use half of the slave resources to get subsequent offer.
  Future<vector<Offer> > offers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver.start();

  AWAIT_READY(offers1);
  EXPECT_NE(0u, offers1.get().size());
  Resources resources1(offers1.get()[0].resources());
  EXPECT_EQ(2, resources1.cpus().get());
  EXPECT_EQ(Megabytes(1024), resources1.mem().get());

  TaskInfo task1;
  task1.set_name("");
  task1.mutable_task_id()->set_value("1");
  task1.mutable_slave_id()->MergeFrom(offers1.get()[0].slave_id());
  task1.mutable_resources()->MergeFrom(halfSlave);
  task1.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);
  vector<TaskInfo> tasks1;
  tasks1.push_back(task1);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status1;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status1));

  Future<vector<Offer> > offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers2));

  // We want to be notified immediately with new offer.
  Filters filters;
  filters.set_refuse_seconds(0);

  driver.launchTasks(offers1.get()[0].id(), tasks1, filters);

  AWAIT_READY(status1);
  EXPECT_EQ(TASK_RUNNING, status1.get().state());

  // Await 2nd offer.
  AWAIT_READY(offers2);
  EXPECT_NE(0u, offers2.get().size());

  Resources resources2(offers2.get()[0].resources());
  EXPECT_EQ(1, resources2.cpus().get());
  EXPECT_EQ(Megabytes(512), resources2.mem().get());

  Future<TaskStatus> status2;
  EXPECT_CALL(exec, killTask(_, _))
    .WillOnce(SendStatusUpdateFromTaskID(TASK_KILLED));

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status2));

  Future<vector<Offer> > offers3;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers3))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Kill 1st task.
  TaskID taskId1 = task1.task_id();
  driver.killTask(taskId1);

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_KILLED, status2.get().state());

  // Await 3rd offer - 2nd and 3rd offer to same slave are now ready.
  AWAIT_READY(offers3);
  EXPECT_NE(0u, offers3.get().size());
  Resources resources3(offers3.get()[0].resources());
  EXPECT_EQ(1, resources3.cpus().get());
  EXPECT_EQ(Megabytes(512), resources3.mem().get());

  TaskInfo task2;
  task2.set_name("");
  task2.mutable_task_id()->set_value("2");
  task2.mutable_slave_id()->MergeFrom(offers2.get()[0].slave_id());
  task2.mutable_resources()->MergeFrom(fullSlave);
  task2.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks2;
  tasks2.push_back(task2);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status3;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status3));

  vector<OfferID> combinedOffers;
  combinedOffers.push_back(offers2.get()[0].id());
  combinedOffers.push_back(offers3.get()[0].id());

  driver.launchTasks(combinedOffers, tasks2);

  AWAIT_READY(status3);
  EXPECT_EQ(TASK_RUNNING, status3.get().state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// Test ensures offers for launchTasks cannot span multiple slaves.
TEST_F(MasterTest, LaunchAcrossSlavesTest)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  // See LaunchCombinedOfferTest() for resource size motivation.
  Resources fullSlave = Resources::parse("cpus:2;mem:1024").get();
  Resources twoSlaves = fullSlave + fullSlave;

  slave::Flags flags = CreateSlaveFlags();

  flags.resources = Option<string>(stringify(fullSlave));

  Try<PID<Slave> > slave1 = StartSlave(&containerizer, flags);
  ASSERT_SOME(slave1);

  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer> > offers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver.start();

  AWAIT_READY(offers1);
  EXPECT_NE(0u, offers1.get().size());
  Resources resources1(offers1.get()[0].resources());
  EXPECT_EQ(2, resources1.cpus().get());
  EXPECT_EQ(Megabytes(1024), resources1.mem().get());

  // Test that offers cannot span multiple slaves.
  Future<vector<Offer> > offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  Try<PID<Slave> > slave2 = StartSlave(&containerizer, flags);
  ASSERT_SOME(slave2);

  AWAIT_READY(offers2);
  EXPECT_NE(0u, offers2.get().size());
  Resources resources2(offers1.get()[0].resources());
  EXPECT_EQ(2, resources2.cpus().get());
  EXPECT_EQ(Megabytes(1024), resources2.mem().get());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers1.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(twoSlaves);
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);
  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  vector<OfferID> combinedOffers;
  combinedOffers.push_back(offers1.get()[0].id());
  combinedOffers.push_back(offers2.get()[0].id());

  Future<Nothing> recoverResources =
    FUTURE_DISPATCH(_, &AllocatorProcess::recoverResources);

  driver.launchTasks(combinedOffers, tasks);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_LOST, status.get().state());
  EXPECT_EQ(TaskStatus::REASON_INVALID_OFFERS, status.get().reason());

  // The resources of the invalid offers should be recovered.
  AWAIT_READY(recoverResources);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// Test ensures that an offer cannot appear more than once in offers
// for launchTasks.
TEST_F(MasterTest, LaunchDuplicateOfferTest)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  // See LaunchCombinedOfferTest() for resource size motivation.
  Resources fullSlave = Resources::parse("cpus:2;mem:1024").get();

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = Option<string>(stringify(fullSlave));

  Try<PID<Slave> > slave = StartSlave(&containerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // Test that same offers cannot be used more than once.
  // Kill 2nd task and get offer for full slave.
  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());
  Resources resources(offers.get()[0].resources());
  EXPECT_EQ(2, resources.cpus().get());
  EXPECT_EQ(Megabytes(1024), resources.mem().get());

  vector<OfferID> combinedOffers;
  combinedOffers.push_back(offers.get()[0].id());
  combinedOffers.push_back(offers.get()[0].id());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(fullSlave);
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);
  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<TaskStatus> status;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  Future<Nothing> recoverResources =
    FUTURE_DISPATCH(_, &AllocatorProcess::recoverResources);

  driver.launchTasks(combinedOffers, tasks);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_LOST, status.get().state());
  EXPECT_EQ(TaskStatus::REASON_INVALID_OFFERS, status.get().reason());

  // The resources of the invalid offers should be recovered.
  AWAIT_READY(recoverResources);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


TEST_F(MasterTest, MetricsInStatsEndpoint)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  Future<process::http::Response> response =
    process::http::get(master.get(), "stats.json");

  AWAIT_READY(response);

  EXPECT_SOME_EQ(
      "application/json",
      response.get().headers.get("Content-Type"));

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);

  ASSERT_SOME(parse);

  JSON::Object stats = parse.get();

  EXPECT_EQ(1u, stats.values.count("master/uptime_secs"));

  EXPECT_EQ(1u, stats.values.count("elected"));
  EXPECT_EQ(1u, stats.values.count("master/elected"));

  EXPECT_EQ(1, stats.values["elected"]);
  EXPECT_EQ(1, stats.values["master/elected"]);

  EXPECT_EQ(1u, stats.values.count("master/slaves_connected"));
  EXPECT_EQ(1u, stats.values.count("master/slaves_disconnected"));
  EXPECT_EQ(1u, stats.values.count("master/slaves_active"));
  EXPECT_EQ(1u, stats.values.count("master/slaves_inactive"));

  EXPECT_EQ(1u, stats.values.count("master/frameworks_connected"));
  EXPECT_EQ(1u, stats.values.count("master/frameworks_disconnected"));
  EXPECT_EQ(1u, stats.values.count("master/frameworks_active"));
  EXPECT_EQ(1u, stats.values.count("master/frameworks_inactive"));

  EXPECT_EQ(1u, stats.values.count("master/outstanding_offers"));

  EXPECT_EQ(1u, stats.values.count("master/tasks_staging"));
  EXPECT_EQ(1u, stats.values.count("master/tasks_starting"));
  EXPECT_EQ(1u, stats.values.count("master/tasks_running"));
  EXPECT_EQ(1u, stats.values.count("master/tasks_finished"));
  EXPECT_EQ(1u, stats.values.count("master/tasks_failed"));
  EXPECT_EQ(1u, stats.values.count("master/tasks_killed"));
  EXPECT_EQ(1u, stats.values.count("master/tasks_lost"));

  EXPECT_EQ(1u, stats.values.count("master/dropped_messages"));

  // Messages from schedulers.
  EXPECT_EQ(1u, stats.values.count("master/messages_register_framework"));
  EXPECT_EQ(1u, stats.values.count("master/messages_reregister_framework"));
  EXPECT_EQ(1u, stats.values.count("master/messages_unregister_framework"));
  EXPECT_EQ(1u, stats.values.count("master/messages_deactivate_framework"));
  EXPECT_EQ(1u, stats.values.count("master/messages_kill_task"));
  EXPECT_EQ(1u, stats.values.count(
      "master/messages_status_update_acknowledgement"));
  EXPECT_EQ(1u, stats.values.count("master/messages_resource_request"));
  EXPECT_EQ(1u, stats.values.count("master/messages_launch_tasks"));
  EXPECT_EQ(1u, stats.values.count("master/messages_decline_offers"));
  EXPECT_EQ(1u, stats.values.count("master/messages_revive_offers"));
  EXPECT_EQ(1u, stats.values.count("master/messages_reconcile_tasks"));
  EXPECT_EQ(1u, stats.values.count("master/messages_framework_to_executor"));

  // Messages from slaves.
  EXPECT_EQ(1u, stats.values.count("master/messages_register_slave"));
  EXPECT_EQ(1u, stats.values.count("master/messages_reregister_slave"));
  EXPECT_EQ(1u, stats.values.count("master/messages_unregister_slave"));
  EXPECT_EQ(1u, stats.values.count("master/messages_status_update"));
  EXPECT_EQ(1u, stats.values.count("master/messages_exited_executor"));

  // Messages from both schedulers and slaves.
  EXPECT_EQ(1u, stats.values.count("master/messages_authenticate"));

  EXPECT_EQ(1u, stats.values.count(
      "master/valid_framework_to_executor_messages"));
  EXPECT_EQ(1u, stats.values.count(
      "master/invalid_framework_to_executor_messages"));

  EXPECT_EQ(1u, stats.values.count("master/valid_status_updates"));
  EXPECT_EQ(1u, stats.values.count("master/invalid_status_updates"));

  EXPECT_EQ(1u, stats.values.count(
      "master/valid_status_update_acknowledgements"));
  EXPECT_EQ(1u, stats.values.count(
      "master/invalid_status_update_acknowledgements"));

  EXPECT_EQ(1u, stats.values.count("master/recovery_slave_removals"));

  EXPECT_EQ(1u, stats.values.count("master/event_queue_messages"));
  EXPECT_EQ(1u, stats.values.count("master/event_queue_dispatches"));
  EXPECT_EQ(1u, stats.values.count("master/event_queue_http_requests"));

  EXPECT_EQ(1u, stats.values.count("master/cpus_total"));
  EXPECT_EQ(1u, stats.values.count("master/cpus_used"));
  EXPECT_EQ(1u, stats.values.count("master/cpus_percent"));

  EXPECT_EQ(1u, stats.values.count("master/mem_total"));
  EXPECT_EQ(1u, stats.values.count("master/mem_used"));
  EXPECT_EQ(1u, stats.values.count("master/mem_percent"));

  EXPECT_EQ(1u, stats.values.count("master/disk_total"));
  EXPECT_EQ(1u, stats.values.count("master/disk_used"));
  EXPECT_EQ(1u, stats.values.count("master/disk_percent"));

  EXPECT_EQ(1u, stats.values.count("registrar/queued_operations"));
  EXPECT_EQ(1u, stats.values.count("registrar/registry_size_bytes"));

  EXPECT_EQ(1u, stats.values.count("registrar/state_fetch_ms"));
  EXPECT_EQ(1u, stats.values.count("registrar/state_store_ms"));

  Shutdown();
}


// This test ensures that when a slave is recovered from the registry
// but does not re-register with the master, it is removed from the
// registry and the framework is informed that the slave is lost, and
// the slave is refused re-registration.
TEST_F(MasterTest, RecoveredSlaveDoesNotReregister)
{
  // Step 1: Start a master.
  master::Flags masterFlags = CreateMasterFlags();
  Try<PID<Master> > master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Step 2: Start a slave.
  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get(), _);

  slave::Flags slaveFlags = this->CreateSlaveFlags();

  // Setup recovery slave flags.
  slaveFlags.checkpoint = true;
  slaveFlags.recover = "reconnect";
  slaveFlags.strict = true;

  Try<PID<Slave> > slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Step 3: Stop the slave while the master is down.
  this->Stop(master.get());

  this->Stop(slave.get());

  // Step 4: Restart the master.
  master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Step 5: Start a scheduler.
  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  driver.start();

  AWAIT_READY(registered);

  // Step 6: Advance the clock until the re-registration timeout
  // elapses, and expect the slave / task to be lost!
  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  Clock::pause();
  Clock::advance(masterFlags.slave_reregister_timeout);

  AWAIT_READY(slaveLost);

  Clock::resume();

  // Step 7: Ensure the slave cannot re-register!
  Future<ShutdownMessage> shutdownMessage =
    FUTURE_PROTOBUF(ShutdownMessage(), master.get(), _);

  slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(shutdownMessage);

  driver.stop();
  driver.join();

  Shutdown();
}


// This test ensures that a non-strict registry is write-only by
// inducing a slave removal during recovery. After which, we expect
// that the framework is *not* informed, and we expect that the
// slave can re-register successfully.
TEST_F(MasterTest, NonStrictRegistryWriteOnly)
{
  // Step 1: Start a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.registry_strict = false;

  Try<PID<Master> > master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Step 2: Start a slave.
  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get(), _);

  slave::Flags slaveFlags = this->CreateSlaveFlags();

  // Setup recovery slave flags.
  slaveFlags.checkpoint = true;
  slaveFlags.recover = "reconnect";
  slaveFlags.strict = true;

  Try<PID<Slave> > slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Step 3: Stop the slave while the master is down.
  this->Stop(master.get());

  this->Stop(slave.get());

  // Step 4: Restart the master.
  master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Step 5: Start a scheduler.
  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return()); // Ignore offers.

  driver.start();

  AWAIT_READY(registered);

  // Step 6: Advance the clock and make sure the slave is not
  // removed!
  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillRepeatedly(FutureSatisfy(&slaveLost));

  Clock::pause();
  Clock::advance(masterFlags.slave_reregister_timeout);
  Clock::settle();

  ASSERT_TRUE(slaveLost.isPending());

  Clock::resume();

  // Step 7: Now expect the slave to be able to re-register,
  // according to the non-strict semantics.
  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), master.get(), _);

  slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveReregisteredMessage);

  driver.stop();
  driver.join();

  Shutdown();
}


// This test ensures that when a slave is recovered from the registry
// and re-registers with the master, it is *not* removed after the
// re-registration timeout elapses.
TEST_F(MasterTest, RecoveredSlaveReregisters)
{
  // Step 1: Start a master.
  master::Flags masterFlags = CreateMasterFlags();
  Try<PID<Master> > master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Step 2: Start a slave.
  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get(), _);

  slave::Flags slaveFlags = this->CreateSlaveFlags();

  // Setup recovery slave flags.
  slaveFlags.checkpoint = true;
  slaveFlags.recover = "reconnect";
  slaveFlags.strict = true;

  Try<PID<Slave> > slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Step 3: Stop the slave while the master is down.
  this->Stop(master.get());

  this->Stop(slave.get());

  // Step 4: Restart the master.
  master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Step 5: Start a scheduler to ensure the master would notify
  // a framework, were a slave to be lost.
  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  // Ignore all offer related calls. The scheduler might receive
  // offerRescinded calls because the slave might re-register due to
  // ping timeout.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .WillRepeatedly(Return());

  driver.start();

  AWAIT_READY(registered);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), master.get(), _);

  slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveReregisteredMessage);

  // Step 6: Advance the clock and make sure the slave is not
  // removed!
  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillRepeatedly(FutureSatisfy(&slaveLost));

  Clock::pause();
  Clock::advance(masterFlags.slave_reregister_timeout);
  Clock::settle();

  ASSERT_TRUE(slaveLost.isPending());

  driver.stop();
  driver.join();

  Shutdown();
}


#ifdef MESOS_HAS_JAVA

class MasterZooKeeperTest : public MesosZooKeeperTest {};

// This test verifies that when the ZooKeeper cluster is lost,
// master, slave & scheduler all get informed.
TEST_F(MasterZooKeeperTest, LostZooKeeperCluster)
{
  ASSERT_SOME(StartMaster());

  Future<process::Message> slaveRegisteredMessage =
    FUTURE_MESSAGE(Eq(SlaveRegisteredMessage().GetTypeName()), _, _);

  ASSERT_SOME(StartSlave());

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, stringify(url.get()), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return()); // Ignore offers.

  Future<process::Message> frameworkRegisteredMessage =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  driver.start();

  // Wait for the "registered" messages so that we know the master is
  // detected by everyone.
  AWAIT_READY(frameworkRegisteredMessage);
  AWAIT_READY(slaveRegisteredMessage);

  Future<Nothing> schedulerDisconnected;
  EXPECT_CALL(sched, disconnected(&driver))
    .WillOnce(FutureSatisfy(&schedulerDisconnected));

  // Need to drop these two dispatches because otherwise the master
  // will EXIT.
  Future<Nothing> masterDetected = DROP_DISPATCH(_, &Master::detected);
  Future<Nothing> lostCandidacy = DROP_DISPATCH(_, &Master::lostCandidacy);

  Future<Nothing> slaveDetected = FUTURE_DISPATCH(_, &Slave::detected);

  server->shutdownNetwork();

  Clock::pause();

  while (schedulerDisconnected.isPending() ||
         masterDetected.isPending() ||
         slaveDetected.isPending() ||
         lostCandidacy.isPending()) {
    Clock::advance(MASTER_CONTENDER_ZK_SESSION_TIMEOUT);
    Clock::settle();
  }

  Clock::resume();

  // Master, slave and scheduler all lose the leading master.
  AWAIT_READY(schedulerDisconnected);
  AWAIT_READY(masterDetected);
  AWAIT_READY(lostCandidacy);
  AWAIT_READY(slaveDetected);

  driver.stop();
  driver.join();

  Shutdown();
}

#endif // MESOS_HAS_JAVA


// This test ensures that when a master fails over, those tasks that
// belong to some currently unregistered frameworks will appear in the
// "orphan_tasks" field in the state.json. And those unregistered frameworks
// will appear in the "unregistered_frameworks" field.
TEST_F(MasterTest, OrphanTasks)
{
  // Start a master.
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  StandaloneMasterDetector detector (master.get());

  // Start a slave.
  Try<PID<Slave> > slave = StartSlave(&exec, &detector);
  ASSERT_SOME(slave);

  // Create a task on the slave.
  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &detector);

  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(SaveArg<1>(&frameworkId))
    .WillRepeatedly(Return()); // Ignore subsequent events.

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 64, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.start();

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  // Get the master's state.
  Future<process::http::Response> response =
    process::http::get(master.get(), "state.json");
  AWAIT_READY(response);

  EXPECT_SOME_EQ(
      "application/json",
      response.get().headers.get("Content-Type"));

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(parse);

  JSON::Object state = parse.get();
  // Record the original framework and task info.
  JSON::Array frameworks =
    state.values["frameworks"].as<JSON::Array>();
  JSON::Object activeFramework =
    frameworks.values.front().as<JSON::Object>();
  JSON::String activeFrameworkId =
    activeFramework.values["id"].as<JSON::String>();
  JSON::Array activeTasks =
    activeFramework.values["tasks"].as<JSON::Array>();
  JSON::Array orphanTasks =
    state.values["orphan_tasks"].as<JSON::Array>();
  JSON::Array unknownFrameworksArray =
    state.values["unregistered_frameworks"].as<JSON::Array>();

  EXPECT_EQ(1u, frameworks.values.size());
  EXPECT_EQ(1u, activeTasks.values.size());
  EXPECT_EQ(0u, orphanTasks.values.size());
  EXPECT_EQ(0u, unknownFrameworksArray.values.size());
  EXPECT_EQ(frameworkId.value(), activeFrameworkId.value);

  EXPECT_CALL(sched, disconnected(&driver))
    .Times(1);

  // Stop the master.
  Stop(master.get());

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), master.get(), _);

  // Drop the reregisterFrameworkMessage to delay the framework
  // from re-registration.
  Future<ReregisterFrameworkMessage> reregisterFrameworkMessage =
    DROP_PROTOBUF(ReregisterFrameworkMessage(), _, master.get());

  Future<FrameworkRegisteredMessage> frameworkRegisteredMessage =
    FUTURE_PROTOBUF(FrameworkRegisteredMessage(), master.get(), _);

  Clock::pause();

  // The master failover.
  master = StartMaster();
  ASSERT_SOME(master);

  // Settle the clock to ensure the master finishes
  // executing _recover().
  Clock::settle();

  // Simulate a new master detected event to the slave and the framework.
  detector.appoint(master.get());

  AWAIT_READY(slaveReregisteredMessage);
  AWAIT_READY(reregisterFrameworkMessage);

  // Get the master's state.
  response = process::http::get(master.get(), "state.json");
  AWAIT_READY(response);

  EXPECT_SOME_EQ(
      "application/json",
      response.get().headers.get("Content-Type"));

  parse = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(parse);

  // Verify that we have some orphan tasks and unregistered
  // frameworks.
  state = parse.get();
  orphanTasks = state.values["orphan_tasks"].as<JSON::Array>();
  EXPECT_EQ(activeTasks, orphanTasks);

  unknownFrameworksArray =
    state.values["unregistered_frameworks"].as<JSON::Array>();
  EXPECT_EQ(1u, unknownFrameworksArray.values.size());

  JSON::String unknownFrameworkId =
    unknownFrameworksArray.values.front().as<JSON::String>();
  EXPECT_EQ(activeFrameworkId, unknownFrameworkId);

  // Advance the clock to let the framework re-register with the master.
  Clock::advance(Seconds(1));
  Clock::settle();
  Clock::resume();

  AWAIT_READY(frameworkRegisteredMessage);

  // Get the master's state.
  response = process::http::get(master.get(), "state.json");
  AWAIT_READY(response);

  EXPECT_SOME_EQ(
      "application/json",
      response.get().headers.get("Content-Type"));

  parse = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(parse);

  // Verify the orphan tasks and unregistered frameworks are removed.
  state = parse.get();
  unknownFrameworksArray =
    state.values["unregistered_frameworks"].as<JSON::Array>();
  EXPECT_EQ(0u, unknownFrameworksArray.values.size());

  orphanTasks = state.values["orphan_tasks"].as<JSON::Array>();
  EXPECT_EQ(0u, orphanTasks.values.size());

  // Cleanup.
  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that the master will strip ephemeral ports
// resource from offers so that frameworks cannot see it.
TEST_F(MasterTest, IgnoreEphemeralPortsResource)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  string resourcesWithoutEphemeralPorts =
    "cpus:2;mem:1024;disk:1024;ports:[31000-32000]";

  string resourcesWithEphemeralPorts =
    resourcesWithoutEphemeralPorts + ";ephemeral_ports:[30001-30999]";

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = resourcesWithEphemeralPorts;

  Try<PID<Slave> > slave = StartSlave(flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  EXPECT_EQ(1u, offers.get().size());

  EXPECT_EQ(
      Resources(offers.get()[0].resources()),
      Resources::parse(resourcesWithoutEphemeralPorts).get());

  driver.stop();
  driver.join();

  Shutdown();
}


#ifdef WITH_NETWORK_ISOLATOR
TEST_F(MasterTest, MaxExecutorsPerSlave)
{
  master::Flags flags = CreateMasterFlags();
  flags.max_executors_per_slave = 0;

  Try<PID<Master> > master = StartMaster(flags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  Try<PID<Slave> > slave = StartSlave(&containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<MasterInfo> masterInfo;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<2>(&masterInfo));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .Times(0);

  driver.start();

  AWAIT_READY(masterInfo);
  EXPECT_EQ(master.get().node.port, masterInfo.get().port());
  EXPECT_EQ(master.get().node.ip, masterInfo.get().ip());

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}
#endif  // WITH_NETWORK_ISOLATOR


// This test verifies that when the Framework has not responded to
// an offer within the default timeout, the offer is rescinded.
TEST_F(MasterTest, OfferTimeout)
{
  master::Flags masterFlags = MesosTest::CreateMasterFlags();
  masterFlags.offer_timeout = Seconds(30);
  Try<PID<Master> > master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Try<PID<Slave> > slave = StartSlave();
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  Future<vector<Offer> > offers1;
  Future<vector<Offer> > offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillOnce(FutureArg<1>(&offers2));

  // Expect offer rescinded.
  Future<Nothing> offerRescinded;
  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .WillOnce(FutureSatisfy(&offerRescinded));

  Future<Nothing> recoverResources =
    FUTURE_DISPATCH(_, &AllocatorProcess::recoverResources);

  driver.start();

  AWAIT_READY(registered);
  AWAIT_READY(offers1);
  ASSERT_EQ(1u, offers1.get().size());

  // Now advance the clock, we need to resume it afterwards to
  // allow the allocator to make a new allocation decision.
  Clock::pause();
  Clock::advance(masterFlags.offer_timeout.get());
  Clock::resume();

  AWAIT_READY(offerRescinded);

  AWAIT_READY(recoverResources);

  // Expect that the resources are re-offered to the framework after
  // the rescind.
  AWAIT_READY(offers2);
  ASSERT_EQ(1u, offers2.get().size());

  EXPECT_EQ(offers1.get()[0].resources(), offers2.get()[0].resources());

  driver.stop();
  driver.join();

  Shutdown();
}


// Offer should not be rescinded if it's accepted.
TEST_F(MasterTest, OfferNotRescindedOnceUsed)
{
  master::Flags masterFlags = MesosTest::CreateMasterFlags();
  masterFlags.offer_timeout = Seconds(30);
  Try<PID<Master> > master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  Try<PID<Slave> > slave = StartSlave(&containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 64, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  // We don't expect any rescinds if the offer has been accepted.
  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(0);

  driver.start();
  AWAIT_READY(registered);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  // Now advance to the offer timeout, we need to settle the clock to
  // ensure that the offer rescind timeout would be processed
  // if triggered.
  Clock::pause();
  Clock::advance(masterFlags.offer_timeout.get());
  Clock::settle();

  driver.stop();
  driver.join();

  Shutdown();
}


// Offer should not be rescinded if it has been declined.
TEST_F(MasterTest, OfferNotRescindedOnceDeclined)
{
  master::Flags masterFlags = MesosTest::CreateMasterFlags();
  masterFlags.offer_timeout = Seconds(30);
  Try<PID<Master> > master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  Try<PID<Slave> > slave = StartSlave(&containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillRepeatedly(DeclineOffers()); // Decline all offers.

  Future<LaunchTasksMessage> launchTasksMessage =
    FUTURE_PROTOBUF(LaunchTasksMessage(), _, _);

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(0);

  driver.start();
  AWAIT_READY(registered);

  // Wait for the framework to decline the offers.
  AWAIT_READY(launchTasksMessage);

  // Now advance to the offer timeout, we need to settle the clock to
  // ensure that the offer rescind timeout would be processed
  // if triggered.
  Clock::pause();
  Clock::advance(masterFlags.offer_timeout.get());
  Clock::settle();

  driver.stop();
  driver.join();

  Shutdown();
}


// This test ensures that the master releases resources for tasks
// when they terminate, even if no acknowledgements occur.
TEST_F(MasterTest, UnacknowledgedTerminalTask)
{
  master::Flags masterFlags = CreateMasterFlags();
  Try<PID<Master> > master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:64";
  Try<PID<Slave> > slave = StartSlave(&containerizer, slaveFlags);
  ASSERT_SOME(slave);

  // Launch a framework and get a task into a terminal state.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers1;
  Future<vector<Offer> > offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(FutureArg<1>(&offers1),
                    LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 64, "*")))
    .WillOnce(FutureArg<1>(&offers2)); // Ignore subsequent offers.

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_FINISHED));

  // Capture the status update message from the slave to the master.
  Future<StatusUpdateMessage> update =
    FUTURE_PROTOBUF(StatusUpdateMessage(), _, master.get());

  // Drop the status updates forwarded to the framework to ensure
  // that the task remains terminal and unacknowledged in the master.
  DROP_PROTOBUFS(StatusUpdateMessage(), master.get(), _);

  driver.start();

  // Wait until the framework is registered.
  AWAIT_READY(frameworkId);
  AWAIT_READY(offers1);

  // Once the update is sent, the master should re-offer the
  // resources consumed by the task.
  AWAIT_READY(update);

  // Don't wait around for the allocation interval.
  Clock::pause();
  Clock::advance(masterFlags.allocation_interval);
  Clock::resume();

  AWAIT_READY(offers2);

  EXPECT_FALSE(offers1.get().empty());
  EXPECT_FALSE(offers2.get().empty());

  // Ensure we get all of the resources back.
  EXPECT_EQ(offers1.get()[0].resources(), offers2.get()[0].resources());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// This test ensures that the master releases resources for a
// terminated task even when it receives a non-terminal update (with
// latest state set).
TEST_F(MasterTest, ReleaseResourcesForTerminalTaskWithPendingUpdates)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:64";
  Try<PID<Slave> > slave = StartSlave(&containerizer, slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 64, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  ExecutorDriver* execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(SaveArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  // Drop all the updates from master to scheduler.
  DROP_PROTOBUFS(StatusUpdateMessage(), master.get(), _);

  Future<StatusUpdateMessage> statusUpdateMessage =
    FUTURE_PROTOBUF(StatusUpdateMessage(), _, master.get());

  Future<Nothing> __statusUpdate = FUTURE_DISPATCH(_, &Slave::__statusUpdate);

  driver.start();

  // Wait until TASK_RUNNING is sent to the master.
  AWAIT_READY(statusUpdateMessage);

  // Ensure status update manager handles TASK_RUNNING update.
  AWAIT_READY(__statusUpdate);

  Future<Nothing> __statusUpdate2 = FUTURE_DISPATCH(_, &Slave::__statusUpdate);

  // Now send TASK_FINISHED update.
  TaskStatus finishedStatus;
  finishedStatus = statusUpdateMessage.get().update().status();
  finishedStatus.set_state(TASK_FINISHED);
  execDriver->sendStatusUpdate(finishedStatus);

  // Ensure status update manager handles TASK_FINISHED update.
  AWAIT_READY(__statusUpdate2);

  Future<Nothing> recoverResources = FUTURE_DISPATCH(
      _, &AllocatorProcess::recoverResources);

  // Advance the clock so that the status update manager resends
  // TASK_RUNNING update with 'latest_state' as TASK_FINISHED.
  Clock::pause();
  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::resume();

  // Ensure the resources are recovered.
  AWAIT_READY(recoverResources);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// This test ensures that the web UI of a framework is included in the
// state.json endpoint, if provided by the framework.
TEST_F(MasterTest, FrameworkWebUIUrl)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_webui_url("http://localhost:8080/");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  driver.start();

  AWAIT_READY(registered);

  Future<process::http::Response> masterState =
    process::http::get(master.get(), "state.json");
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, masterState);

  Try<JSON::Object> masterStateObject =
    JSON::parse<JSON::Object>(masterState.get().body);
  ASSERT_SOME(masterStateObject);

  // We need a mutable copy of masterStateObject to use [].
  JSON::Object masterStateObject_ = masterStateObject.get();

  EXPECT_EQ(1u, masterStateObject_.values.count("frameworks"));
  JSON::Array frameworks =
    masterStateObject_.values["frameworks"].as<JSON::Array>();

  EXPECT_EQ(1u, frameworks.values.size());
  JSON::Object framework_ = frameworks.values.front().as<JSON::Object>();

  EXPECT_EQ(1u, framework_.values.count("webui_url"));
  JSON::String webui_url =
    framework_.values["webui_url"].as<JSON::String>();

  EXPECT_EQ("http://localhost:8080/", webui_url.value);

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that label values are exposed over the master
// state endpoint.
TEST_F(MasterTest, TaskLabels)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  Try<PID<Slave>> slave = StartSlave(&containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  // Add three labels to the task (two of which share the same key).
  Labels* labels = task.mutable_labels();

  Label* label1 = labels->add_labels();
  label1->set_key("foo");
  label1->set_value("bar");

  Label* label2 = labels->add_labels();
  label2->set_key("bar");
  label2->set_value("baz");

  Label* label3 = labels->add_labels();
  label3->set_key("bar");
  label3->set_value("qux");

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<Nothing> update;
  EXPECT_CALL(containerizer,
              update(_, Resources(offers.get()[0].resources())))
    .WillOnce(DoAll(FutureSatisfy(&update),
                    Return(Future<Nothing>())));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  AWAIT_READY(update);

  // Verify label key and value in master state.json.
  Future<process::http::Response> response =
    process::http::get(master.get(), "state.json");
  AWAIT_READY(response);

  EXPECT_SOME_EQ(
      "application/json",
      response.get().headers.get("Content-Type"));

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(parse);

  Result<JSON::Array> labelsObject = parse.get().find<JSON::Array>(
      "frameworks[0].tasks[0].labels");
  EXPECT_SOME(labelsObject);

  JSON::Array labelsObject_ = labelsObject.get();

  // Verify the content of 'foo:bar' pair.
  Try<JSON::Value> expected = JSON::parse(
      "{"
      "  \"key\":\"foo\","
      "  \"value\":\"bar\""
      "}");

  ASSERT_SOME(expected);
  EXPECT_EQ(labelsObject_.values[0], expected.get());


  // Verify the content of 'bar:baz' pair.
  expected = JSON::parse(
      "{"
      "  \"key\":\"bar\","
      "  \"value\":\"baz\""
      "}");

  ASSERT_SOME(expected);
  EXPECT_EQ(labelsObject_.values[1], expected.get());


  // Verify the content of 'bar:qux' pair.
  expected = JSON::parse(
      "{"
      "  \"key\":\"bar\","
      "  \"value\":\"qux\""
      "}");

  ASSERT_SOME(expected);
  EXPECT_EQ(labelsObject_.values[2], expected.get());


  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// This tests the 'active' field in slave entries from state.json. We
// first verify an active slave, deactivate it and verify that the
// 'active' field is false.
TEST_F(MasterTest, SlaveActiveEndpoint)
{
  // Start a master.
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<process::Message> slaveRegisteredMessage =
    FUTURE_MESSAGE(Eq(SlaveRegisteredMessage().GetTypeName()), _, _);

  // Start a checkpointing slave.
  slave::Flags flags = CreateSlaveFlags();
  flags.checkpoint = true;
  Try<PID<Slave>> slave = StartSlave(flags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Verify slave is active.
  Future<process::http::Response> response =
    process::http::get(master.get(), "state.json");
  AWAIT_READY(response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(parse);

  Result<JSON::Boolean> status = parse.get().find<JSON::Boolean>(
      "slaves[0].active");

  ASSERT_SOME_EQ(JSON::Boolean(true), status);

  Future<Nothing> deactivateSlave =
    FUTURE_DISPATCH(_, &AllocatorProcess::deactivateSlave);

  // Inject a slave exited event at the master causing the master
  // to mark the slave as disconnected.
  process::inject::exited(slaveRegisteredMessage.get().to, master.get());

  // Wait until master deactivates the slave.
  AWAIT_READY(deactivateSlave);

  // Verify slave is inactive.
  response = process::http::get(master.get(), "state.json");
  AWAIT_READY(response);

  parse = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(parse);

  status = parse.get().find<JSON::Boolean>("slaves[0].active");

  ASSERT_SOME_EQ(JSON::Boolean(false), status);

  Shutdown();
}


// This test verifies that service info for tasks is exposed over the
// master state endpoint.
TEST_F(MasterTest, TaskDiscoveryInfo)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  Try<PID<Slave>> slave = StartSlave(&containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task;
  task.set_name("testtask");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offers.get()[0].slave_id());
  task.mutable_resources()->CopyFrom(offers.get()[0].resources());
  task.mutable_executor()->CopyFrom(DEFAULT_EXECUTOR_INFO);

  // An expanded service discovery info to the task.
  DiscoveryInfo* info = task.mutable_discovery();
  info->set_visibility(DiscoveryInfo::EXTERNAL);
  info->set_name("mytask");
  info->set_environment("mytest");
  info->set_location("mylocation");
  info->set_version("v0.1.1");

  // Add two named ports to the discovery info.
  Ports* ports = info->mutable_ports();
  Port* port1 = ports->add_ports();
  port1->set_number(8888);
  port1->set_name("myport1");
  port1->set_protocol("tcp");
  Port* port2 = ports->add_ports();
  port2->set_number(9999);
  port2->set_name("myport2");
  port2->set_protocol("udp");

  // Add two labels to the discovery info.
  Labels* labels = info->mutable_labels();
  Label* label1 = labels->add_labels();
  label1->set_key("clearance");
  label1->set_value("high");
  Label* label2 = labels->add_labels();
  label2->set_key("RPC");
  label2->set_value("yes");

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<Nothing> update;
  EXPECT_CALL(containerizer,
              update(_, Resources(offers.get()[0].resources())))
    .WillOnce(DoAll(FutureSatisfy(&update),
                    Return(Future<Nothing>())));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  AWAIT_READY(update);

  // Verify label key and value in master state.json.
  Future<process::http::Response> response =
    process::http::get(master.get(), "state.json");
  AWAIT_READY(response);

  EXPECT_SOME_EQ(
      "application/json",
      response.get().headers.get("Content-Type"));

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(parse);

  Result<JSON::String> taskName = parse.get().find<JSON::String>(
      "frameworks[0].tasks[0].name");
  EXPECT_SOME(taskName);
  ASSERT_EQ("testtask", taskName.get());

  // Verify basic content for discovery info.
  Result<JSON::String> visibility = parse.get().find<JSON::String>(
      "frameworks[0].tasks[0].discovery.visibility");
  EXPECT_SOME(visibility);
  DiscoveryInfo::Visibility visibility_value;
  DiscoveryInfo::Visibility_Parse(visibility.get().value, &visibility_value);
  ASSERT_EQ(DiscoveryInfo::EXTERNAL, visibility_value);

  Result<JSON::String> discoveryName = parse.get().find<JSON::String>(
      "frameworks[0].tasks[0].discovery.name");
  EXPECT_SOME(discoveryName);
  ASSERT_EQ("mytask", discoveryName.get());

  Result<JSON::String> environment = parse.get().find<JSON::String>(
      "frameworks[0].tasks[0].discovery.environment");
  EXPECT_SOME(environment);
  ASSERT_EQ("mytest", environment.get());

  Result<JSON::String> location = parse.get().find<JSON::String>(
      "frameworks[0].tasks[0].discovery.location");
  EXPECT_SOME(location);
  ASSERT_EQ("mylocation", location.get());

  Result<JSON::String> version = parse.get().find<JSON::String>(
      "frameworks[0].tasks[0].discovery.version");
  EXPECT_SOME(version);
  ASSERT_EQ("v0.1.1", version.get());

  // Verify content of two named ports.
  Result<JSON::Array> portsArray = parse.get().find<JSON::Array>(
      "frameworks[0].tasks[0].discovery.ports.ports");
  EXPECT_SOME(portsArray);

  JSON::Array portsArray_ = portsArray.get();
  EXPECT_EQ(2u, portsArray_.values.size());

  // Verify the content of '8888:myport1:tcp' port.
  Try<JSON::Value> expected = JSON::parse(
      "{"
      "  \"number\":8888,"
      "  \"name\":\"myport1\","
      "  \"protocol\":\"tcp\""
      "}");
  ASSERT_SOME(expected);
  EXPECT_EQ(expected.get(), portsArray_.values[0]);

  // Verify the content of '9999:myport2:udp' port.
  expected = JSON::parse(
      "{"
      "  \"number\":9999,"
      "  \"name\":\"myport2\","
      "  \"protocol\":\"udp\""
      "}");
  ASSERT_SOME(expected);
  EXPECT_EQ(expected.get(), portsArray_.values[1]);

  // Verify content of two labels.
  Result<JSON::Array> labelsArray = parse.get().find<JSON::Array>(
      "frameworks[0].tasks[0].discovery.labels.labels");
  EXPECT_SOME(labelsArray);

  JSON::Array labelsArray_ = labelsArray.get();
  EXPECT_EQ(2u, labelsArray_.values.size());

  // Verify the content of 'clearance:high' pair.
  expected = JSON::parse(
      "{"
      "  \"key\":\"clearance\","
      "  \"value\":\"high\""
      "}");
  ASSERT_SOME(expected);
  EXPECT_EQ(expected.get(), labelsArray_.values[0]);

  // Verify the content of 'RPC:yes' pair.
  expected = JSON::parse(
      "{"
      "  \"key\":\"RPC\","
      "  \"value\":\"yes\""
      "}");
  ASSERT_SOME(expected);
  EXPECT_EQ(expected.get(), labelsArray_.values[1]);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}
