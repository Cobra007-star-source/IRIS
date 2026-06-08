-- wrk pipelining script (mirrors the TechEmpower /plaintext methodology:
-- 16 pipelined requests per write). Usage:
--   wrk -t4 -c256 -d15s -s scripts/pipeline.lua http://host:8080/plaintext
local depth = tonumber(os.getenv("PIPELINE_DEPTH") or "16")
local r = {}
for i = 1, depth do
  r[i] = wrk.format("GET")
end
req = table.concat(r)

function init(args)
  wrk.headers["Host"] = "localhost"
end

function request()
  return req
end
