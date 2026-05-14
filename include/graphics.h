#ifndef GRAPHICS_H
#define GRAPHICS_H

/* ============================================================
 * graphics.h
 * ------------------------------------------------------------
 * Declares the entry point of the OpenGL/GLUT display loop
 * that runs in its own forked process.
 *
 * يصرّح عن نقطة دخول حلقة العرض البصري المبنية على OpenGL/GLUT
 * والتي تعمل في عملية ابن مفروقة بشكل مستقل.
 * ============================================================ */

#include "common.h"

/* Start the GLUT main loop in the calling process; reads from
 * SharedState continuously to render the current state. Never
 * returns (runs until the parent kills it via SIGTERM).
 *
 * يبدأ حلقة GLUT الرئيسية في العملية المنادية، ويقرأ باستمرار
 * من بنية الحالة المشتركة لرسم الوضع الحالي. لا يرجع أبداً
 * (يعمل حتى يقتله الأب بإشارة SIGTERM). */
void graphics_run(SharedState *state);

#endif /* GRAPHICS_H */
