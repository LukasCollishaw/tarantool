env = require('test_run')
test_run = env.new()
test_run:cmd("push filter '(.builtin/.*.lua):[0-9]+' to '\\1'")

-- gh-2025 box.savepoint

s1 = nil
s1 = box.savepoint()
box.rollback_to_savepoint(s1)

box.begin() s1 = box.savepoint()
box.rollback()

box.begin() box.rollback_to_savepoint(s1)
box.rollback()

engine = test_run:get_cfg('engine')

-- Test many savepoints on each statement.
s = box.schema.space.create('test', {engine = engine})
p = s:create_index('pk')
test_run:cmd("setopt delimiter ';'")
box.begin()
s:replace{1}
save1 = box.savepoint()
s:replace{2}
save2 = box.savepoint()
s:replace{3}
save3 = box.savepoint()
s:replace{4}
select1 = s:select{}
box.rollback_to_savepoint(save3)
select2 = s:select{}
box.rollback_to_savepoint(save2)
select3 = s:select{}
box.rollback_to_savepoint(save1)
select4 = s:select{}
box.commit()
test_run:cmd("setopt delimiter ''");
select1
select2
select3
select4
s:truncate()

-- Test rollback to savepoint on the current statement.
test_run:cmd("setopt delimiter ';'")
box.begin()
s:replace{1}
s:replace{2}
s1 = box.savepoint()
box.rollback_to_savepoint(s1)
box.commit()
test_run:cmd("setopt delimiter ''");
s:select{}
s:truncate()

-- Test rollback to savepoint after multiple statements.
test_run:cmd("setopt delimiter ';'")
box.begin()
s:replace{1}
s1 = box.savepoint()
s:replace{2}
s:replace{3}
s:replace{4}
box.rollback_to_savepoint(s1)
box.commit()
test_run:cmd("setopt delimiter ''");
s:select{}
s:truncate()

-- Test rollback to savepoint after failed statement.
test_run:cmd("setopt delimiter ';'")
box.begin()
s:replace{1}
s1 = box.savepoint()
s:replace{3}
pcall(s.replace, s, {'kek'})
s:replace{4}
box.rollback_to_savepoint(s1)
box.commit()
test_run:cmd("setopt delimiter ''");
s:select{}
s:truncate()

-- Test rollback to savepoint inside the trigger.
select1 = nil
select2 = nil
select3 = nil
select4 = nil
test_run:cmd("setopt delimiter ';'")
function on_replace(old, new)
	if new[1] > 10 then return end
	select1 = s:select{}
	s1 = box.savepoint()
	s:replace{100}
	box.rollback_to_savepoint(s1)
	select2 = s:select{}
end;
_ = s:on_replace(on_replace);
box.begin()
s:replace{1}
select3 = select1
select4 = select2
s:replace{2}
box.commit()
test_run:cmd("setopt delimiter ''");
select4
select3
select2
select1
s:select{}
s:drop()

-- Test rollback to savepoint, created in trigger,
-- from main tx stream. Fail, because of different substatement
-- levels.
s = box.schema.space.create('test', {engine = engine})
p = s:create_index('pk')
test_run:cmd("setopt delimiter ';'")
function on_replace2(old, new)
	if new[1] ~= 1 then return end
	s1 = box.savepoint()
	s:replace{100}
end;
_ = s:on_replace(on_replace2);
box.begin()
s:replace{1}
select1 = s:select{}
s:replace{2}
s:replace{3}
select2 = s:select{}
ok1, errmsg1 = pcall(box.rollback_to_savepoint, s1)
select3 = s:select{}
s:replace{4}
select4 = s:select{}
box.commit()
test_run:cmd("setopt delimiter ''");
select1
select2
select3
select4
ok1
errmsg1
s:drop()

-- Test incorrect savepoints usage inside a transaction.
s = box.schema.space.create('test', {engine = engine})
p = s:create_index('pk')

test_run:cmd("setopt delimiter ';'")
box.begin()
s1 = box.savepoint()
txn_id = s1.txn_id
s:replace{1}
ok1, errmsg1 = pcall(box.rollback_to_savepoint)
ok2, errmsg2 = pcall(box.rollback_to_savepoint, {txn_id=txn_id})
ok3, errmsg3 = pcall(box.rollback_to_savepoint, {txn_id=txn_id, csavepoint=100})
fake_cdata = box.tuple.new({txn_id})
ok4, errmsg4 = pcall(box.rollback_to_savepoint, {txn_id=txn_id, csavepoint=fake_cdata})
ok5, errmsg5 = pcall(box.rollback_to_savepoint, {txn_id=fake_cdata, csavepoint=s1.csavepoint})
box.commit()
test_run:cmd("setopt delimiter ''");
ok1, errmsg1
ok2, errmsg2
ok3, errmsg3
ok4, errmsg4
ok5, errmsg5
s:select{}

-- Rollback to released savepoint.
box.begin() ok1, errmsg1 = pcall(box.rollback_to_savepoint, s1) box.commit()
ok1, errmsg1
s:select{}

s:truncate()

-- Rollback several savepoints at once.
test_run:cmd("setopt delimiter ';'")
box.begin()
s0 = box.savepoint()
s:replace{1}

s1 = box.savepoint()
s:replace{2}

s2 = box.savepoint()
s:replace{3}

s3 = box.savepoint()
s:replace{4}

s4 = box.savepoint()
s:replace{5}
select1 = s:select{}

box.rollback_to_savepoint(s2)
select2 = s:select{}
ok1, errmsg1 = pcall(box.rollback_to_savepoint, s3)
select3 = s:select{}

s5 = box.savepoint()
s:replace{6}

s6 = box.savepoint()
s:replace{7}
select4 = s:select{}
ok2, errmsg2 = pcall(box.rollback_to_savepoint, s4)
select5 = s:select{}

box.rollback_to_savepoint(s6)
select6 = s:select{}

box.rollback_to_savepoint(s0)
select7 = s:select{}
box.rollback()
test_run:cmd("setopt delimiter ''");
select1
select2
select3
select4
select5
select6
select7
ok1, errmsg1
ok2, errmsg2
s:truncate()

-- Rollback to the same substatement level, but from different
-- context.
test_run:cmd("setopt delimiter ';'")
function on_replace3(old_tuple, new_tuple)
	if new_tuple[2] == 'create savepoint' then
		s1 = box.savepoint()
	elseif new_tuple[2] == 'rollback to savepoint' then
		box.rollback_to_savepoint(s1)
	end
end;
_ = s:on_replace(on_replace3);
box.begin()
s:replace{1, 'create savepoint'}
s:replace{2}
s:replace{3}
s:replace{4, 'rollback to savepoint'}
s:replace{5}
box.commit()
test_run:cmd("setopt delimiter ''");
s:select{}
s:truncate()

-- Several savepoints on a same statement.
test_run:cmd("setopt delimiter ';'")
box.begin()
s:replace{1}
s1 = box.savepoint()
s2 = box.savepoint()
s3 = box.savepoint()
s:replace{2}
box.rollback_to_savepoint(s3)
box.rollback_to_savepoint(s2)
box.rollback_to_savepoint(s1)
box.commit()
test_run:cmd("setopt delimiter ''");
s:select{}
s:truncate()

-- Test multiple rollback of a same savepoint.
test_run:cmd("setopt delimiter ';'")
box.begin()
s1 = box.savepoint()
s:replace{1}
box.rollback_to_savepoint(s1)
s:replace{2}
box.rollback_to_savepoint(s1)
s:replace{3}
box.commit()
test_run:cmd("setopt delimiter ''");
s:select{}

s:drop()
