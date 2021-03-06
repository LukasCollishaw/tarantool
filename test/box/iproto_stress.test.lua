test_run = require('test_run').new()

fiber = require('fiber')
net_box = require('net.box')

net_msg_max = box.cfg.net_msg_max
box.cfg{net_msg_max = 16}

box.schema.user.grant('guest', 'read,write,execute', 'universe')

s = box.schema.space.create('test')
_ = s:create_index('primary', {unique=true, parts={1, 'unsigned', 2, 'unsigned', 3, 'unsigned'}})

n_errors = 0
n_workers = 0

test_run:cmd("setopt delimiter ';'")
function worker(i)
    n_workers = n_workers + 1
    for j = 1,2 do
        local status, conn = pcall(net_box.connect, box.cfg.listen)
        if not status then
            n_errors = n_errors + 1
            break
        end
        for k = 1,10 do
            conn.space.test:insert{i, j, k}
        end
        conn:close()
        fiber.sleep(0.1)
    end
    n_workers = n_workers - 1
end;
test_run:cmd("setopt delimiter ''");

for i = 1, 100 do fiber.create(worker, i) end
fiber.sleep(0.1)

-- check that iproto doesn't deplete tx fiber pool on wal stall (see gh-1892)
box.error.injection.set("ERRINJ_WAL_DELAY", true)
fiber.sleep(0.1)
box.error.injection.set("ERRINJ_WAL_DELAY", false)

test_run:wait_cond(function() return n_workers == 0 end, 60)
n_workers -- 0
n_errors -- 0

box.schema.user.revoke('guest', 'read,write,execute', 'universe')
s:drop()

box.cfg{net_msg_max = net_msg_max}
