test_run = require('test_run').new()
engine = test_run:get_cfg('engine')
replica_set = require('fast_replica')
fiber = require('fiber')

test_run:cleanup_cluster()

-- Make each snapshot trigger garbage collection.
default_checkpoint_count = box.cfg.checkpoint_count
box.cfg{checkpoint_count = 1}

-- Grant permissions needed for replication.
box.schema.user.grant('guest', 'read,write,execute', 'universe')
box.schema.user.grant('guest', 'replication')

-- By default, relay thread reports status to tx once a second.
-- To reduce the test execute time, let's set it to 50 ms.
box.error.injection.set("ERRINJ_RELAY_REPORT_INTERVAL", 0.05)

-- Create and populate the space we will replicate.
s = box.schema.space.create('test', {engine = engine});
_ = s:create_index('pk')
for i = 1, 100 do s:auto_increment{} end
box.snapshot()
for i = 1, 100 do s:auto_increment{} end

-- Make sure replica join will take long enough for us to
-- invoke garbage collection.
box.error.injection.set("ERRINJ_RELAY_TIMEOUT", 0.05)

-- While the replica is receiving the initial data set,
-- make a snapshot and invoke garbage collection, then
-- remove the timeout injection so that we don't have to
-- wait too long for the replica to start.
test_run:cmd("setopt delimiter ';'")
fiber.create(function()
    fiber.sleep(0.1)
    box.snapshot()
    box.error.injection.set("ERRINJ_RELAY_TIMEOUT", 0)
end)
test_run:cmd("setopt delimiter ''");

-- Start the replica.
test_run:cmd("create server replica with rpl_master=default, script='replication/replica.lua'")
test_run:cmd("start server replica")

-- Despite the fact that we invoked garbage collection that
-- would have normally removed the snapshot the replica was
-- bootstrapped from, the replica should still receive all
-- data from the master. Check it.
test_run:cmd("switch replica")
fiber = require('fiber')
while box.space.test:count() < 200 do fiber.sleep(0.01) end
box.space.test:count()
test_run:cmd("switch default")

-- Check that garbage collection removed the snapshot once
-- the replica released the corresponding checkpoint.
fiber.sleep(0.1) -- wait for relay to notify tx
#box.internal.gc.info().checkpoints == 1 or box.internal.gc.info()

-- Make sure the replica will receive data it is subscribed
-- to long enough for us to invoke garbage collection.
box.error.injection.set("ERRINJ_RELAY_TIMEOUT", 0.05)

-- Send more data to the replica.
for i = 1, 100 do s:auto_increment{} end

-- Invoke garbage collection. Check that it doesn't remove
-- xlogs needed by the replica.
box.snapshot()
#box.internal.gc.info().checkpoints == 2 or box.internal.gc.info()

-- Remove the timeout injection so that the replica catches
-- up quickly.
box.error.injection.set("ERRINJ_RELAY_TIMEOUT", 0)

-- Check that the replica received all data from the master.
test_run:cmd("switch replica")
while box.space.test:count() < 300 do fiber.sleep(0.01) end
box.space.test:count()
test_run:cmd("switch default")

-- Now garbage collection should resume and delete files left
-- from the old checkpoint.
fiber.sleep(0.1) -- wait for relay to notify tx
#box.internal.gc.info().checkpoints == 1 or box.internal.gc.info()

-- Stop the replica.
test_run:cmd("stop server replica")
test_run:cmd("cleanup server replica")

-- Invoke garbage collection. Check that it doesn't remove
-- the checkpoint last used by the replica.
_ = s:auto_increment{}
box.snapshot()
#box.internal.gc.info().checkpoints == 2 or box.internal.gc.info()

-- The checkpoint should only be deleted after the replica
-- is unregistered.
test_run:cleanup_cluster()
#box.internal.gc.info().checkpoints == 1 or box.internal.gc.info()

-- Cleanup.
s:drop()
box.error.injection.set("ERRINJ_RELAY_REPORT_INTERVAL", 0)
box.schema.user.revoke('guest', 'replication')
box.schema.user.revoke('guest', 'read,write,execute', 'universe')

box.cfg{checkpoint_count = default_checkpoint_count}
