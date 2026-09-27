--!strict
-- Negative fixture: every line here is wrong in a way the definition file must
-- catch. CI asserts that this file FAILS to type-check; if it ever passes, the
-- definition file has silently become `any` and is no longer protecting
-- anything.

mcode.tool.register({ name = 42 })
mcode.fs.read(123)
mcode.spawn({ "ls" }, { timeout_ms = "soon" })
mcode.not_a_real_function()
mcode.notify()
local bad: string = mcode.api_version
