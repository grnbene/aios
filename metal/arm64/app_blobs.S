/* The bundled face apps (apps/*.aos), embedded so the Pi's in-memory root
 * has them. The desktop's Apps window listed these four and could open none:
 * the files lived only in the repository. kernel_boot.cpp installs them into
 * <root>/apps/ at first boot. */
.section .rodata
.balign 16
.globl app_order_desk_start
app_order_desk_start: .incbin "apps/order_desk.aos"
.globl app_order_desk_end
app_order_desk_end:
.balign 16
.globl app_task_tracker_start
app_task_tracker_start: .incbin "apps/task_tracker.aos"
.globl app_task_tracker_end
app_task_tracker_end:
.balign 16
.globl app_system_monitor_start
app_system_monitor_start: .incbin "apps/system_monitor.aos"
.globl app_system_monitor_end
app_system_monitor_end:
.balign 16
.globl app_notes_start
app_notes_start: .incbin "apps/notes.aos"
.globl app_notes_end
app_notes_end:
