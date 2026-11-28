#ifndef ZCS_ZCS_H
#define ZCS_ZCS_H

/**
 * @file zcs.h
 * @brief Zero Media Server 全量聚合头（demo / 内部方便用）。
 *
 * 嵌入与插件宿主请用 zcs/sdk.h（Stable），不要依赖本头的「全家桶」包含。
 * 进阶场景可优先使用领域聚合头：zcs/live.h、zcs/vod.h、zcs/player.h、zcs/protocol.h。
 *
 * 分层：docs/stream-pipeline.md
 *
 * Copyright (c) zero-media-server
 */
#include "zcs/version.h"
#include "zcs/zcs_export.h"
#include "zcs/server.h"
#include "zcs/core/frame.h"
#include "zcs/core/module_registry.h"
#include "zcs/core/stream/hub.h"
#include "zcs/core/event.h"
#include "zcs/protocol.h"
#include "zcs/session.h"
#include "zcs/live.h"
#include "zcs/vod.h"
#include "zcs/player.h"
#include "zcs/ops/config/config.h"
#include "zcs/ops/api/webapi/webapi.h"

#endif /* ZCS_ZCS_H */
