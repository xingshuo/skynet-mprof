local Skynet = require "skynet"
local Netpack = require "skynet.netpack"
local Socketdriver = require "skynet.socketdriver"
local queue     -- message queue
local CMD = setmetatable({}, { __gc = function() Netpack.clear(queue) end })

local LisFd

local MSG = {}

local totalRecvBytes = 0

local function dispatch_msg(fd, msg, sz)
    totalRecvBytes = totalRecvBytes + sz
    Skynet.error(string.format("gate recv: %d, total: %d", sz, totalRecvBytes))
    -- Notice: memory leak here!
    -- Skynet.trash(msg, sz)
end

MSG.data = dispatch_msg

local function dispatch_queue()
    local fd, msg, sz = Netpack.pop(queue)
    if fd then
        Skynet.fork(dispatch_queue)
        dispatch_msg(fd, msg, sz)

        for fd, msg, sz in Netpack.pop, queue do
            dispatch_msg(fd, msg, sz)
        end
    end
end

MSG.more = dispatch_queue

function MSG.open(fd, msg)
    Socketdriver.start(fd)
    Socketdriver.nodelay(fd)
end

function MSG.close(fd)
end

function MSG.error(fd, msg)
end

function MSG.init(fd, msg)
end

Skynet.register_protocol {
    name = "socket",
    id = Skynet.PTYPE_SOCKET,   -- PTYPE_SOCKET = 6
    unpack = function ( msg, sz )
        return Netpack.filter( queue, msg, sz)
    end,
    dispatch = function (_, _, q, type, ...)
        queue = q
        if type then
            MSG[type](...)
        end
    end
}

Skynet.start(function()
    Skynet.dispatch("lua", function (_, _, cmd, ...)
        local f = assert(CMD[cmd], cmd)
        Skynet.retpack(f(...))
    end)

    local port = assert(tonumber(Skynet.getenv("gate_port")))
    local address = "0.0.0.0"
    Skynet.error(string.format("====Listen on %s:%d start====", address, port))
    lisFd = Socketdriver.listen(address, port)
    Socketdriver.start(lisFd)
    Skynet.error(string.format("====Listen on %s:%d %d end====", address, port, lisFd))
    Skynet.error("====service test_gate start====")
end)