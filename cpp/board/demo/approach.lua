-- name: 接近瞄准
--
-- approach.lua — **基础动作**：追到目标 → 对准 → **停在那儿，不夹**。
--
-- 与 grab.lua 的唯一区别就是最后那一步：这里到位即停并返回，不做夹取、也不等
-- ZP10S 那套"伸下去→夹→抬起"的 4 秒。适合"先看看对准得准不准"、或者后面接人工/别的
-- 动作（比如对准之后由人来按夹爪）。
--
-- 动作脚本是**通用**的：跟哪个模型无关，模型由宿主从卡片配置里注入到 params.model
-- （见 cpp/README.md 的"动作 × 模型"一节）。所以同一份脚本对所有模型都能跑，
-- 想换行为就再加一份动作脚本，不用给每个模型复制一遍。
--
-- 用法:
--   POST /api/demo/init {"name":"追网球"}                      ← 跑界面上的卡片
--   POST /api/demo/init {"action":"grab","model":"tennis"}     ← 直接指定，不用建卡
--   POST /api/demo/run
--   {"script":"grab", "max_seconds":30, "params":{"model":"tennis","target_size":300}}
--
-- 参数（从 params() 里读；卡片里那份存在 demo/configs/<卡片名>.json）:
--   model        必填，用哪个模型找目标（demo/models/<model>.cvimodel）
--   target_size  必填，目标框宽（原图像素）；框宽达到它就认为到位
--   speed        直线速度百分比，默认 20（宿主还会再 clamp 到 ≤70）
--   turn_speed   转弯速度百分比，默认跟直线一样
--
-- 这套判据与参数照搬隔壁仓库 aka0 那个预编译追物 demo 的源码（实机调过参）：
-- 面积最大的框当目标 → 偏出居中带先原地转（脉冲时长与偏离成正比）
-- → 太近就退一小段 → 对准但还不够大就前进一小段 → 够大且居中就抓。四处不同：
--   1. 用框宽像素判定（本项目的口径），不是框面积占比；
--   2. 不做 demo 那种"抓前左转 3 次"的爪子偏置补偿（实测夹空再加）；
--   3. 丢目标就收工，不做没有超时的原地找球；
--   4. 凑太近（框宽超过目标 1.5 倍）会先退一小段，而不是硬贴上去。
--
-- 安全：这里没有、也不可能有"解除限速"的办法 —— 速度、总时长、内存、以及
-- "人的指令一进来就必须交还控制权"全在宿主（capp/script.cpp）里强制。

local p = params() or {}
-- 模型来自卡片配置 / 请求参数（动作脚本通用，不写死）。缺了就直接说清楚 ——
-- 不 then 的话 detect() 会拿 nil 去开模型，报错很难懂。
local model = p.model
if type(model) ~= "string" or model == "" then
    fail("params.model 必填：这张卡片要用哪个模型找目标")
end
local target = tonumber(p.target_size)
local speed = tonumber(p.speed) or 20                 -- 直线速度
local turn_speed = tonumber(p.turn_speed) or speed    -- 转弯速度（没配就跟直线一样）

if not target or target <= 0 then fail("params.target_size 必须是正数（目标框宽，像素）") end

-- 判据常量（改这里就是调参，不用重编）
local CENTER_MARGIN = 80       -- 走的时候用：偏出这个范围就来一次大转向
local TURN_PULSE_K  = 0.5      -- 转向脉冲系数：ms/px（偏离越多转越久）
-- 转向脉冲下限：**同样必须大于电机启动时间**（板上实测 ~250ms）。原来写 25ms，
-- 结果"精调"连着触发 4 次偏移纹丝不动 —— 脉冲太短，电机根本没转起来。
local TURN_PULSE_MIN = 300
local TURN_PULSE_MAX = 400
-- 前进脉冲：**必须大于电机启动时间**。板上实测这块底盘要 ~250ms 才转得起来，
-- 150ms 的脉冲每次都在"刚要转"时被 standby 停掉 → 车只抖不走（轮速全程 0）。
-- 600ms 下 4 秒就走到位。
local FWD_PULSE_MS  = 600
local ALIGN_MARGIN  = 25       -- 抓的时候用：要对到 GRAB_OFFSET 附近这么小的范围才敢夹
-- 太近的判据：框宽超过目标的这个倍数 = 凑得太近。原来没有这一支 —— 目标离得太近时脚本
-- 只会"到位"或原地精调，**从不后退**，于是车几乎贴上目标，夹爪反而够不着/视野全被占满。
local TOO_CLOSE_K   = 1.5
-- 后退脉冲：跟转向脉冲一个思路 —— **按"超出到位区间多少像素"成比例**（超得越多退越久），
-- 不是拍一个固定毫秒数。上下限都留着：
--   BACK_PULSE_MIN 必须大于电机启动时间（板上实测 ~250ms，短了只会在原地抖，车不动）；
--   BACK_PULSE_MAX 防止退过头 —— 车尾没有眼睛，退多了更麻烦。
local BACK_PULSE_K   = 2.0     -- ms/px（超出的像素数）
local BACK_PULSE_MIN = 300
local BACK_PULSE_MAX = 700
-- **爪子对准的是它自己，不是画面中心**。夹爪装在相机右侧，所以要夹准，目标应当出现在
-- 画面中心的**右侧**（正偏移）。原来的版本一直往画面中心对，实测抓的那一刻偏移是 -76px
-- （球在中心左边），正好对到夹爪的反方向 —— 这就是"左右没对准"的原因。
-- 这个值只能实测调：先给 50，看夹取效果再增减（改这个数不用重编）。
local GRAB_OFFSET   = 50
local FINE_PULSE_MAX = 500     -- 精调转向的脉冲上限（够大但没对准时用，别转过头）
local LOST_MS       = 1500     -- 连续多久看不到目标就收工
local LOOP_GAP_MS   = 30       -- 每步之间喘口气，让相机出新帧

-- 注意：算出来的值都可能带小数（框宽/偏移尤其），%d 遇到浮点会直接报错把脚本弄死 ——
-- 显示用的数一律 math.floor 一下（这正是"叠框"那行踩过的坑）。
log("开始：模型=%s 目标框宽=%dpx 直线速度=%d%% 转弯速度=%d%%",
    model, math.floor(target), math.floor(speed), math.floor(turn_speed))

local last_seen = elapsed_ms()

while true do
    local d, err = detect(model)
    if not d then
        -- 推理本身失败（模型没了/相机不可用）——重试没意义，直接收工
        fail("推理失败：" .. tostring(err))
    end

    -- 选面积最大的框（最近的优先，与 demo 一致）
    local best = nil
    for _, b in ipairs(d.boxes) do
        if not best or b.area > best.area then best = b end
    end

    if not best then
        local lost = elapsed_ms() - last_seen
        note("lost_ms", lost)
        if lost > LOST_MS then
            standby()
            return "目标丢失（" .. lost .. "ms 没看到目标）"
        end
        standby()                 -- 丢帧就原地等，不搜索
        sleep_ms(LOOP_GAP_MS)
    else
        last_seen = elapsed_ms()
        local w = best.x2 - best.x1
        local offset = best.cx - d.frame_w / 2      -- 正 = 目标偏右
        note("box_w", math.floor(w))
        note("offset", math.floor(offset))

        -- 转向方向一律是"把目标送到 GRAB_OFFSET 那个位置"：目标偏左就左转（视角随之右移）
        local align_err = offset - GRAB_OFFSET
        if w > target * TOO_CLOSE_K then
            -- 太近：退一小段再重新判。**退多久按超出的像素数算**（跟转向脉冲同一个套路），
            -- 所以"刚过线"退一点点、"贴脸了"退大步
            local over = w - target * TOO_CLOSE_K
            local back_ms = math.floor(BACK_PULSE_K * over)
            back_ms = math.max(BACK_PULSE_MIN, math.min(BACK_PULSE_MAX, back_ms))
            back(speed)
            sleep_ms(back_ms)
            standby()
            log("太近：框宽 %dpx（超出到位区间 %dpx）→ 后退 %dms",
                math.floor(w), math.floor(over), back_ms)
        elseif w >= target and math.abs(align_err) <= ALIGN_MARGIN then
            -- 够大 + 对准夹爪 → 停稳，**不夹就走人**（这就是这个动作的全部）
            brake()
            log("到位：框宽 %dpx（目标 %d）偏移 %d（夹爪位 %d，误差 %d）→ 只接近，不抓",
                math.floor(w), math.floor(target), math.floor(offset), GRAB_OFFSET,
                math.floor(align_err))
            return string.format("已到位（未抓取）：框宽 %dpx，偏移 %d，夹爪位 %d",
                                 math.floor(w), math.floor(offset), GRAB_OFFSET)
        elseif math.abs(offset) > CENTER_MARGIN then
            -- 还差得远：大脉冲转向
            local pulse = math.floor(TURN_PULSE_K * math.abs(align_err))
            pulse = math.max(TURN_PULSE_MIN, math.min(TURN_PULSE_MAX, pulse))
            if align_err < 0 then turn_left(turn_speed) else turn_right(turn_speed) end
            sleep_ms(pulse)
            standby()
        elseif w >= target then
            -- 距离够了但没对准夹爪：小脉冲精调（别转过头）
            local pulse = math.floor(TURN_PULSE_K * math.abs(align_err))
            pulse = math.max(TURN_PULSE_MIN, math.min(FINE_PULSE_MAX, pulse))
            log("精调：偏移 %d → 对准 %d（误差 %d，脉冲 %dms）",
                math.floor(offset), GRAB_OFFSET, math.floor(align_err), pulse)
            if align_err < 0 then turn_left(turn_speed) else turn_right(turn_speed) end
            sleep_ms(pulse)
            standby()
        else
            forward(speed)
            sleep_ms(FWD_PULSE_MS)
            standby()
        end
        sleep_ms(LOOP_GAP_MS)
    end
end
