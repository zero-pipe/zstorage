/**
 * @file test_egress_pacing.c
 * @brief 出站 pacing 策略：每 tick 帧预算与滞后分级。
 *
 * 为什么测它：`zms_egress_live_pump_budget` 决定单次 IO tick 发多少帧，
 * 直接关系到「首屏快不快」和「慢消费者会不会拖垮其它会话」。它是纯函数，
 * 但此前没有测试；一旦预算算错，表现是首屏卡顿或突发打爆带宽。
 *
 * 预算语义（见 egress_live_policy.h）：
 *   - 未锁 epoch / 未完成 catchup / 仍落后 → 用突发预算（尽快追平）
 *   - 已追平稳态 → 收敛到稳态预算（防止一次发太多）
 *   - 调用方传入的 caller_budget 优先，但稳态时不得超过稳态上限
 */
#include "zms/egress/pacing/egress_live_policy.h"
#include "zms/egress/pacing/egress_pacing.h"

#include "zms_test.h"


int main(void)
{
    zms_egress_pacing p;

    zms_egress_pacing_defaults(&p);
    zms_egress_pacing_init(&p);

    /* 1) 稳态（epoch 已锁 + catchup 完成 + 未落后）：预算收敛到 live 上限 */
    {
        int b = zms_egress_live_pump_budget(1, 1, 0, p.burst_budget);

        CHECK(b > 0);
        CHECK(b <= (int)p.frame_budget_live);
    }

    /* 2) 未锁 epoch：给突发预算，让首帧尽快出去 */
    {
        int b = zms_egress_live_pump_budget(0, 0, 0, 0);

        CHECK(b == (int)p.burst_budget);
    }

    /* 3) catchup 未完成：同样走突发预算 */
    {
        int b = zms_egress_live_pump_budget(1, 0, 0, 0);

        CHECK(b == (int)p.burst_budget);
    }

    /* 4) 直播稳态下滞后变大：预算仍被钳到稳态值。
     *    这是设计意图——一旦 catchup_done，滞后由 prep 里的 snap/resync 修正，
     *    而不是靠单 tick 突发把积压一次性推出去（那会瞬间打爆带宽）。
     *    测试记录该语义，避免将来被误改成"滞后就突发"。 */
    {
        int b = zms_egress_live_pump_budget(1, 1, p.ring_max_lag + 100u, 0);

        CHECK(b == (int)p.frame_budget_live);
    }

    /* 5) 稳态下调用方给的预算更小：尊重调用方（不能反向放大） */
    {
        int b = zms_egress_live_pump_budget(1, 1, 0, 3);

        CHECK(b == 3);
    }

    /* 6) 滞后判定边界：等于阈值不算 catchup，超过才算 */
    CHECK(zms_egress_live_lag_catchup(p.catchup_lag) == 0);
    CHECK(zms_egress_live_lag_catchup(p.catchup_lag + 1u) != 0);

    /* 7) 参数收敛：非法配置不得让预算变成 0 或负数（否则永远发不出帧） */
    {
        zms_egress_pacing bad;

        memset(&bad, 0, sizeof(bad)); /* 全 0：模拟配置缺失 */
        zms_egress_pacing_init(&bad);
        CHECK(zms_egress_live_pump_budget(1, 1, 0, 0) > 0);
        CHECK(zms_egress_live_pump_budget(0, 0, 0, 0) > 0);
    }

    /* 8) 默认值之间的关系必须自洽（改默认值时这里会先炸） */
    CHECK(p.frame_budget_catchup >= p.frame_budget_live);
    CHECK(p.ring_max_lag >= p.catchup_lag);
    CHECK(p.resync_lag_max >= p.ring_max_lag);
    CHECK(p.pace_lead_ms > 0);

    printf("test_egress_pacing: OK (live=%d catchup=%d burst=%d)\n", p.frame_budget_live,
           p.frame_budget_catchup, p.burst_budget);
    return 0;
}
