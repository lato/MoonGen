---------------------------------
--- @file memoryDevice.lua
--- @brief High level device/NIC/port management for Netmap
--- @todo feature level of DPDK
---------------------------------

local netmapc = require "netmapc"
local ffi = require "ffi"
local log = require "log"
local memory = require "netmapMemory"
local ethtool = require "netmapEthtool"

local mod = {} -- local module

function mod.nextSlot(cur, numSlots)
	if (cur+1) == numSlots then
		return 0
	else
		return cur +1
	end
end

----------------------------------------------------------------------------------
---- Netmap Device
----------------------------------------------------------------------------------

local dev = {}
dev.__index = dev
mod.devices = {}

local txQueue = {}
txQueue.__index = txQueue

local rxQueue = {}
rxQueue.__index = rxQueue

--- Configures a device
--- @param args.port: Interface name (eg. eth1)
--- @todo there are way more parameters with DPDK
--- @return Netmap device object
function mod.config(...)
	local args = {...}
	if type(args[1]) ~= "table" then
		log:fatal("Currently only tables are supported in netmapDevice.config()")
		return nil
	end
	args = args[1]

	-- set default values
	args.txQueues = args.txQueues or 1
	args.rxQueues = args.rxQueues or 1

	local config = ffi.new("struct nm_config_struct[1]")
	config[0].port = args.port
	config[0].txQueues = args.txQueues
	config[0].rxQueues = args.rxQueues

	ethtool.setRingCount(args.port, math.max(args.txQueues, args.rxQueues))
	ethtool.setRingSize(args.port, 2048) -- smaller does not make a lot of sense
	ethtool.setLinkUp(args.port)
	ethtool.waitForLink(args.port)

	-- create new device object
	local dev_ret = {}
	setmetatable(dev_ret, dev)
	dev_ret.port = args.port
	dev_ret.c = netmapc.nm_config(config)

	if dev_ret.c == nil then
		log:fatal("Something went wrong during netmapc.nm_config")
	end

	return dev_ret
end

--- Get a already configured device
--- @param iface: interface name (eg. eth1)
--- @return Netmap device object
function mod.get(port)
	local dev_ret = {}
	setmetatable(dev_ret, dev)
	dev_ret.c = netmapc.nm_get(port)
	dev_ret.port = port
	return dev_ret
end

--- Wait for the links to come up
function dev:wait()
	-- waiting is already done in mod.config()
	return
	--ffi.cdef[[
	--	int sleep(int seconds);
	--]]
	--ffi.C.sleep(5)
end

function mod.waitForLinks()
	-- waiting is already done in mod.config()
	return
	--ffi.cdef[[
	--	int sleep(int seconds);
	--]]
	--ffi.C.sleep(5)
end

--- Get the tx queue with a certain number
--- @param id: index of the queue
--- @return Netmap tx queue object
function dev:getTxQueue(id)
	if tonumber(self.c.nm_ring[id].fd) == 0 then
		log:fatal("This queue was not configured")
	end

	local queue = {}
	setmetatable(queue, txQueue)
	queue.nmRing = netmapc.NETMAP_TXRING_wrapper(self.c.nm_ring[id].nifp, id)
	queue.fd = self.c.nm_ring[id].fd
	queue.dev = self
	queue.id = id
	queue.tx = true

	return queue
end

--- Get the rx queue with a certain number
--- @param id: index of the queue
--- @return Netmap rx queue object
function dev:getRxQueue(id)
	if tonumber(self.c.nm_ring[id].fd) == 0 then
		log:fatal("This queue was not configured")
	end

	local queue = {}
	setmetatable(queue, rxQueue)
	queue.nmRing = netmapc.NETMAP_RXRING_wrapper(self.c.nm_ring[id].nifp, id)
	queue.fd = self.c.nm_ring[id].fd
	queue.dev = self
	queue.id = id
	queue.tx = false

	return queue
end

function dev:getMacString()
	local mac = ffi.new("char[18]")
	local interfaceName = ffi.new("char[17]")
	ffi.copy(interfaceName, self.port)
	local ret = netmapc.get_mac(interfaceName, mac)
	if ret ~= 0 then
		log:fatal("something went wrong in dev:getMacString()")
	end
	return ffi.string(mac)
end

function dev:getTxStats()
	return tonumber(netmapc.fetch_tx_pkts(self.c)), tonumber(netmapc.fetch_tx_octetts(self.c))
end

function dev:getRxStats()
	return tonumber(netmapc.fetch_rx_pkts(self.c)), tonumber(netmapc.fetch_rx_octetts(self.c))
end

function dev:__tostring()
	return ("[Device: interface=%s]"):format(self.port)
end

function dev:__serialize()
	log:debug("dev:__serialize() iface=" .. self.port)
	return ('local dev= require "netmapDevice" return dev.get(%s)'):format(self.port), true
end


----------------------------------------------------------------------------------
---- Netmap Tx Queue
----------------------------------------------------------------------------------

--- Sync the SW view with the HW view
function txQueue:sync()
	netmapc.ioctl_NIOCTXSYNC(self.fd)
end

--- Send the current buffer
--- @param bufs: packet buffers to send
function txQueue:send(bufs)
	if bufs.queue.tx == false then -- we are currently forwarding a packet
		-- swap all the indices and mbuf data pointers
		-- probably a C job
		while bufs.size > self:avail() do
			self:sync()
		end
		netmapc.swap_bufs(bufs.size, self.dev.c, self.id, bufs.queue.dev.c, bufs.queue.id)
		self:sync()
		return -- do not commence beyond the if
	end
	local cur = bufs.first
	netmapc.slot_mbuf_update(self.dev.c, self.id, bufs.first, bufs.size);
	-- syncing and actually sending out packets is too costly
	-- done in alloc of bufArray...
end

--- Return how many slots are available to the userspace
--- @return Number of available buffers
function txQueue:avail()
	ret = self.nmRing.tail - self.nmRing.cur
	if ret < 0 then
		ret = ret + self.nmRing.num_slots
	end
	return ret
end

function txQueue:setRate(rate)
	-- do nothing for now
	-- is something like this supported at all?
end

function txQueue:__tostring()
	log:debug("txQueue:__tostring(): self.dev.iface=" .. self.port .. ", id=" .. self.id)
	return("[TxQueue: interface=%s, ringid=%d"):format(self.dev.port, self.id)
end

function txQueue:__serialize()
	return ('local dev = require "netmapDevice" return dev.get("%s"):getTxQueue(%d)'):format(self.dev.port, self.id), true
end

----------------------------------------------------------------------------------
---- Netmap Rx Queue
----------------------------------------------------------------------------------

function rxQueue:sync()
	netmapc.ioctl_NIOCRXSYNC(self.fd)
end

function rxQueue:recv(bufs)
	self:sync()
	while self:avail() < 1 do
		self:sync()
	end
	bufs.first = self.nmRing.head
	if self:avail() < bufs.maxSize then
		bufs.size = self:avail()
	else
		bufs.size = bufs.maxSize
	end
	netmapc.mbufs_slots_update(self.dev.c, self.id, bufs.first, bufs.size)

	return bufs.size
end

function rxQueue:avail()
	ret = self.nmRing.tail - self.nmRing.cur
	if ret < 0 then
		ret = ret + self.nmRing.num_slots
	end
	return ret
end

function rxQueue:bufArray(s)
	if self.bufs == nil then
		log:debug("creating new mempool for receiving")
		local mem = memory.createMemPool({queue = self})
		self.bufs = mem:bufArray(s)
	end
	return self.bufs
end

function rxQueue:__tostring()
	log:debug("rxQueue:__tostring(): self.dev.iface=" .. self.port .. ", id=" .. self.id)
	return("[RxQueue: interface=%s, ringid=%d"):format(self.dev.port, self.id)
end

function rxQueue:__serialize()
	return ('local dev = require "netmapDevice" return dev.get("%s"):getRxQueue(%d)'):format(self.dev.port, self.id), true
end


return mod
