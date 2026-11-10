/* GraniteC: Granite's agent kernel and the granite-agent/1 standard in C. Everything an author
 * links to write an agent: include this, declare packages (granitec/package.h), and make the
 * runner's main a roster (granitec/server.h):
 *
 *     static const granite_package *const roster[] = {&parrot, NULL};
 *     int main(int argc, char **argv) { return granite_server_main(argc, argv, roster); }
 *
 * Build against it with `pkg-config --cflags --libs granitec`. */
#ifndef GRANITEC_H
#define GRANITEC_H

#include "granitec/answer.h"
#include "granitec/buf.h"
#include "granitec/context.h"
#include "granitec/emitter.h"
#include "granitec/error.h"
#include "granitec/json.h"
#include "granitec/lock.h"
#include "granitec/main.h"
#include "granitec/object.h"
#include "granitec/package.h"
#include "granitec/schema.h"
#include "granitec/thread.h"
#include "granitec/wire.h"

#endif
