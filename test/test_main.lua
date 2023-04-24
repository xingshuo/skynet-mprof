local Skynet = require "skynet"
require "skynet.manager"

Skynet.start(function()
    local handle = Skynet.launch("mprof")
    assert(handle, "launch mprof service failed")
    Skynet.name(".mprof", handle)

    Skynet.newservice("test_gate")
    Skynet.newservice("test_client")
    
    Skynet.exit()
end)