uuid = require('uuid')
test_run = require('test_run').new()

replication_timeout = box.cfg.replication_timeout
replication_connect_timeout = box.cfg.replication_connect_timeout
box.cfg{replication_timeout=0.05, replication_connect_timeout=0.05, replication={}}

-- gh-3160 - Send heartbeats if there are changes from a remote master only
SERVERS = { 'autobootstrap1', 'autobootstrap2', 'autobootstrap3' }

-- Deploy a cluster.
test_run:create_cluster(SERVERS, "replication", {args="0.03"})
test_run:wait_fullmesh(SERVERS)
test_run:cmd("switch autobootstrap3")
test_run = require('test_run').new()
fiber = require('fiber')
_ = box.schema.space.create('test_timeout'):create_index('pk')
test_run:cmd("setopt delimiter ';'")
function wait_not_follow(replicaA, replicaB)
    return test_run:wait_cond(function()
        return replicaA.status ~= 'follow' or replicaB.status ~= 'follow'
    end, box.cfg.replication_timeout)
end;
function test_timeout()
    local replicaA = box.info.replication[1].upstream or box.info.replication[2].upstream
    local replicaB = box.info.replication[3].upstream or box.info.replication[2].upstream
    local follows = test_run:wait_cond(function()
        return replicaA.status == 'follow' or replicaB.status == 'follow'
    end)
    if not follows then error('replicas are not in the follow status') end
    for i = 0, 99 do
        box.space.test_timeout:replace({1})
        if wait_not_follow(replicaA, replicaB) then
            return error(box.info.replication)
        end
    end
    return true
end;
test_run:cmd("setopt delimiter ''");
test_timeout()

test_run:cmd("switch default")
test_run:drop_cluster(SERVERS)
test_run:cleanup_cluster()

box.cfg{replication = "", replication_timeout = replication_timeout, \
        replication_connect_timeout = replication_connect_timeout}
