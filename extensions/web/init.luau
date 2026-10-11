--!strict
-- The web tools: web_search and fetch.
--
-- The canonical network example, and deliberately policy-free. Every
-- security-relevant decision is on the C++ side of the boundary: the permission
-- gate is per domain and knows which extension is calling, the egress proxy
-- carries the domain allowlist, and `mcode.fetch` refuses private and link-local
-- addresses before it connects. Nothing here can reach around any of that, and
-- nothing here should try -- the harness supplies guarantees, this supplies
-- ergonomics.

local search = mcode.net.search
local fetch = mcode.fetch

-- Search is absent when no provider is configured, so the tool is not registered
-- at all. A tool that exists only to fail is worse than a tool that is not there.
local have_search = search ~= nil

local registered_search: boolean = true

if have_search then
	registered_search = pcall(function()
		mcode.tool.register({
			name = "web_search",
			description = "Search the web. Returns titles, URLs and snippets. Use it "
				.. "when the answer is not in the repository: a current API, a "
				.. "library's changelog, an error you have not seen before. Follow up "
				.. "with fetch on the result that looks most relevant.",
			permission = "net",
			schema = {
				["type"] = "object",
				properties = {
					query = {
						["type"] = "string",
						description = "What to search for",
					},
				},
				required = { "query" },
			},
			run = function(args: { [string]: any }, _context: {
				session: string,
				seq: number,
				extension: string,
			}): (string?, string?)
				local query = args.query

				if type(query) ~= "string" or query == "" then
					return nil, "query is required and must be a non-empty string"
				end

				local results = search(query)

				if results == nil then
					return nil, "the search provider is unavailable in this session"
				end

				if #results == 0 then
					return "no results for '" .. query .. "'", nil
				end

				local lines: { string } = {}

				for index, hit in results do
					lines[#lines + 1] = tostring(index) .. ". " .. hit.title
						.. "\n   " .. hit.url

					if hit.snippet ~= nil and hit.snippet ~= "" then
						lines[#lines + 1] = "   " .. hit.snippet
					end
				end

				return table.concat(lines, "\n"), nil
			end,
		})
	end)

	if not registered_search then
		mcode.log.error("web_search could not be registered")
	end
end

local registered_fetch: boolean = true

if fetch ~= nil then
	registered_fetch = pcall(function()
		mcode.tool.register({
			name = "fetch",
			description = "Fetch a URL and return its text. Use it on a search "
				.. "result, or on a documentation page whose URL you already know. "
				.. "Non-text responses are refused rather than mangled.",
			permission = "net",
			schema = {
				["type"] = "object",
				properties = {
					url = {
						["type"] = "string",
						description = "The absolute http or https URL to fetch",
					},
				},
				required = { "url" },
			},
			run = function(args: { [string]: any }, _context: {
				session: string,
				seq: number,
				extension: string,
			}): (string?, string?)
				local url = args.url

				if type(url) ~= "string" or url == "" then
					return nil, "url is required and must be a non-empty string"
				end

				local response = fetch(url)

				if response == nil then
					return nil, "fetch is unavailable in this session"
				end

				if response.status < 200 or response.status >= 400 then
					return nil, "HTTP " .. tostring(response.status) .. " for " .. url
				end

				local body = response.body

				if body == nil or body == "" then
					return "HTTP " .. tostring(response.status) .. " with an empty body", nil
				end

				if response.truncated then
					body = body .. "\n[truncated: the page exceeded the fetch budget]"
				end

				return "HTTP " .. tostring(response.status) .. " " .. response.content_type
					.. "\n\n" .. body, nil
			end,
		})
	end)

	if not registered_fetch then
		mcode.log.error("fetch could not be registered")
	end
end

if not have_search and fetch == nil then
	mcode.log.warn(
		"no search provider is configured and fetch is disabled, so the web "
			.. "extension provides no tools"
	)
end
