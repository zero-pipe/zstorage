#ifndef ZMS_ZMS_H
#define ZMS_ZMS_H

/**
 * @file zms.h
 * @brief Zero Media Server 全量聚合头（demo / 内部方便用）。
 *
 * 嵌入与插件宿主请用 zms/sdk.h（Stable），不要依赖本头的「全家桶」包含。
 * 进阶场景可优先使用领域聚合头：zms/live.h、zms/vod.h、zms/player.h、zms/protocol.h。
 *
 * 分层：docs/stream-pipeline.md
 *
 * Copyright (c) zero-media-server
 */
#include "zms/version.h"
#include "zms/zms_export.h"
#include "zms/server.h"
#include "zms/core/frame.h"
#include "zms/core/module_registry.h"
#include "zms/core/stream/hub.h"
#include "zms/core/event.h"
#include "zms/protocol.h"
#include "zms/session.h"
#include "zms/live.h"
#include "zms/vod.h"
#include "zms/player.h"
#include "zms/ops/config/config.h"
#include "zms/ops/api/webapi/webapi.h"

#endif /* ZMS_ZMS_H */
