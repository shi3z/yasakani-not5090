#!/usr/bin/env python3
"""Smoke test + streaming speed of the OpenAI-compatible server (speed = (tokens - 1) / time from first to last content chunk)."""
import json, sys, time, urllib.request
URL = "http://127.0.0.1:%s/v1/chat/completions" % (sys.argv[1] if len(sys.argv) > 1 else "8765")
def run(name, body):
    body = dict(body, stream=True, stream_options={"include_usage": True})
    req = urllib.request.Request(URL, json.dumps(body).encode(), {"Content-Type": "application/json"})
    t_send = time.time(); t_first = t_last = None; usage = {}; text = []; fin = None
    with urllib.request.urlopen(req, timeout=600) as r:
        for line in r:
            line = line.decode().strip()
            if not line.startswith("data: ") or line == "data: [DONE]": continue
            j = json.loads(line[6:])
            if j.get("usage"): usage = j["usage"]
            for c in j.get("choices", []):
                d = c.get("delta", {})
                if d.get("content") or d.get("reasoning_content") or d.get("tool_calls"):
                    t_last = time.time(); t_first = t_first or t_last; text.append(d.get("content") or "")
                fin = c.get("finish_reason") or fin
    n = usage.get("completion_tokens", 0)
    print("%-22s prompt %5d  out %5d  ttft %.2fs  %7.1f tok/s  finish=%s  | %s" % (name, usage.get("prompt_tokens", 0), n, t_first - t_send,
          (n - 1) / (t_last - t_first) if t_last > t_first else float("nan"), fin, "".join(text)[:60].replace("\n", " ")), flush=True)
off = {"chat_template_kwargs": {"enable_thinking": False}, "temperature": 0}
run("hello (think off)", dict(off, messages=[{"role": "user", "content": "こんにちは。自己紹介を2文でお願いします。"}], max_tokens=256))
run("code (think off)", dict(off, messages=[{"role": "user", "content": "build me a html three.js website for harry potter"}], max_tokens=2048))
run("essay ja (think on)", dict(messages=[{"role": "user", "content": "量子コンピュータの仕組みを、高校生にもわかるように800字くらいで説明してください。"}], max_tokens=4096, seed=1))
run("tool call", dict(off, messages=[{"role": "user", "content": "How is the weather in Osaka?"}], max_tokens=256,
    tools=[{"type": "function", "function": {"name": "get_weather", "description": "Get the weather of a city.", "parameters": {"type": "object", "properties": {"city": {"type": "string"}}, "required": ["city"]}}}]))
