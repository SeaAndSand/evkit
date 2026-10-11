--[[
  evkit — Linux 事件循环 + 队列 FSM

  include/evkit/   对外头文件
  src/core/        → libevkit_core
  src/plugins/     → libevkit_bus / pool / work / task
  tests/           单元测试
  examples/        示例（含 demo 官方模板）
]]

set_project("evkit")
set_version("1.0.0")
set_xmakever("2.8.0")
set_languages("c11")
set_defaultmode("debug")

add_rules("mode.debug", "mode.release")
add_rules("plugin.compile_commands.autoupdate", { outputdir = "." })

set_warnings("all", "extra")
add_syslinks("pthread")

if is_mode("debug") then
    set_symbols("debug")
    set_optimize("none")
end

-- 对外头文件：#include "evkit/….h"
add_includedirs("include", { public = true })

option("examples")
    set_default(true)
    set_showmenu(true)
    set_description("Build example programs")

--------------------------------------------------------------------------------
-- 库
--------------------------------------------------------------------------------

target("evkit_core")
    set_kind("static")
    add_files("src/core/*.c")
    add_headerfiles("include/evkit/ev_loop.h", "include/evkit/ev_fsm.h")

target("evkit_bus")
    set_kind("static")
    add_deps("evkit_core")
    add_files("src/plugins/ev_bus.c")
    add_headerfiles("include/evkit/ev_bus.h")

target("evkit_pool")
    set_kind("static")
    add_deps("evkit_core")
    add_files("src/plugins/ev_pool.c")
    add_headerfiles("include/evkit/ev_pool.h")

target("evkit_work")
    set_kind("static")
    add_deps("evkit_core")
    add_files("src/plugins/ev_work.c")
    add_headerfiles("include/evkit/ev_work.h")

target("evkit_task")
    set_kind("static")
    add_deps("evkit_core", "evkit_work", "evkit_pool")
    add_files("src/plugins/ev_task.c")
    add_headerfiles("include/evkit/ev_task.h")

target("evkit")
    set_kind("phony")
    add_deps("evkit_core", "evkit_bus", "evkit_pool", "evkit_work", "evkit_task")
    set_default(true)

--------------------------------------------------------------------------------
-- 测试
--------------------------------------------------------------------------------

target("evkit_test")
    set_kind("binary")
    add_deps("evkit_core", "evkit_bus", "evkit_pool", "evkit_work", "evkit_task")
    add_files("tests/*.c")
    add_includedirs("tests")
    set_default(false)
    set_group("test")
    add_tests("default")

--------------------------------------------------------------------------------
-- 示例（聚焦 evkit 能力，一条学习路径）
--------------------------------------------------------------------------------

local function example_core(name, dir, default)
    target(name)
        set_kind("binary")
        add_deps("evkit_core")
        add_files(path.join(dir, "main.c"))
        set_default(default or false)
        set_group("examples")
        if not has_config("examples") then
            set_enabled(false)
        end
end

local function example_bus(name, dir)
    target(name)
        set_kind("binary")
        add_deps("evkit_core", "evkit_bus")
        add_files(path.join(dir, "main.c"))
        set_default(false)
        set_group("examples")
        if not has_config("examples") then
            set_enabled(false)
        end
end

example_core("evkit_demo", "examples/demo", true)
example_core("worker_ping", "examples/worker_ping")
example_core("tcp_reconnect", "examples/tcp_reconnect")
example_core("nested_timeout", "examples/nested_timeout")
example_core("upgrade_link", "examples/upgrade_link")
example_bus("topic_hub", "examples/topic_hub")

target("task_ping")
    set_kind("binary")
    add_deps("evkit_core", "evkit_task")
    add_files("examples/task_ping/main.c")
    set_default(false)
    set_group("examples")
    if not has_config("examples") then
        set_enabled(false)
    end
