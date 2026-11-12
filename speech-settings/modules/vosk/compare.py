import json, re, subprocess, sys
ref = json.load(open("../vosk-ref/out/ref.json"))
ok = True
for i, r in enumerate(ref):
    out = open(f"out/trace{i}.txt").read()
    toks = re.search(r"^tokens: (.*)$", out, re.M).group(1).split(" ")
    ids = [int(t.rsplit("/", 1)[1]) for t in toks]
    names = [t.rsplit("/", 1)[0] for t in toks]
    rows = int(re.search(r"^bert_rows: (\d+)", out, re.M).group(1))
    ph = json.loads("[" + re.search(r"^phoneme_ids: (.*)$", out, re.M).group(1).replace("] [", "],[") + "]")
    res = [names == r["tokens"], ids == r["token_ids"], rows == r["bert_rows"], ph == r["phoneme_ids"]]
    print(i, r["text"], res, re.search(r"^load.*$", out, re.M).group(0))
    if not all(res):
        ok = False
        if ph != r["phoneme_ids"]:
            for k, (a, b) in enumerate(zip(ph, r["phoneme_ids"])):
                if a != b: print("   first diff at", k, a, b); break
            print("   len", len(ph), len(r["phoneme_ids"]))
        if names != r["tokens"]: print("   ", names, "\n   ", r["tokens"])
print("ALL OK" if ok else "MISMATCH")
