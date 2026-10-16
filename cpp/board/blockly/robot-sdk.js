// API v1. Every effect crosses the parent service's run/ownership checks.
const robot = (() => {
  let calls = 0;
  let depth = 0;
  const invoke = (method, ...args) => _robotCall(method, args);
  const finite = (value, min, max, label) => {
    const number = Number(value);
    if (!Number.isFinite(number) || number < min || number > max) throw Error(`${label}必须在 ${min}～${max} 之间`);
    return number;
  };
  const positiveSeconds = value => finite(value, 0.1, 30, "移动秒数");
  return Object.freeze({
    forward: async cm => invoke("forward", finite(cm, 0.1, 500, "移动厘米")),
    backward: async cm => invoke("backward", finite(cm, 0.1, 500, "移动厘米")),
    turnAngle: async (dir, degrees) => {
      if (!["left", "right"].includes(dir)) throw Error("转向无效");
      return invoke("turnAngle", dir, finite(degrees, 1, 360, "转向角度"));
    },
    forwardSeconds: async seconds => invoke("forwardSeconds", positiveSeconds(seconds)),
    backwardSeconds: async seconds => invoke("backwardSeconds", positiveSeconds(seconds)),
    turn: async (dir, seconds) => {
      if (!["left", "right"].includes(dir)) throw Error("转向无效");
      return invoke("turn", dir, positiveSeconds(seconds));
    },
    grab: async () => invoke("grab"),
    release: async () => invoke("release"),
    wait: async seconds => invoke("wait", finite(seconds, 0, 60, "等待秒数")),
    print: async value => invoke("print", String(typeof value === "object" ? JSON.stringify(value) : value).slice(0, 2048)),
    stopped: () => invoke("stopped"),
    highlightBlock: async id => invoke("highlightBlock", String(id).slice(0, 128)),
    loopTick: async () => invoke("checkpoint"),
    loopYield: async () => invoke("yield"),
    loopDone: async () => invoke("checkpoint"),
    emptyLoop: async () => invoke("yield"),
    loopLimit: async () => { throw Error("循环超过 100 次"); },
    repeatLimit: async () => { throw Error("重复次数无效或超过 100 次"); },
    loopInputError: async message => { throw Error(String(message)); },
    procedureEnter: async () => {
      if (++calls > 1000 || ++depth > 50) throw Error("函数调用次数或递归深度超过上限");
      invoke("checkpoint");
    },
    procedureExit: () => { depth = Math.max(0, depth - 1); },
    dataGet: (value, key) => {
      key = String(key ?? "").trim();
      if (!key || key.length > 80 || ["__proto__", "prototype", "constructor", "x", "z", "points", "point"].includes(key)) throw Error("字段名称无效");
      return value && typeof value === "object" && Object.prototype.hasOwnProperty.call(value, key) ? value[key] : null;
    },
    listItem: (list, index) => {
      index = Number(index);
      if (!Array.isArray(list) || !Number.isSafeInteger(index) || index < 1) throw Error("列表或序号无效");
      return list[index - 1] ?? null;
    },
    requireList: list => { if (!Array.isArray(list)) throw Error("需要列表数据"); return list; },
    dataLength: value => Array.isArray(value) || typeof value === "string" ? value.length : value && typeof value === "object" ? Object.keys(value).length : 0,
    jsonText: value => JSON.stringify(value)
  });
})();
