/**
 * \file   premake_main.c
 * \brief  Program entry point.
 * \author Copyright (c) 2002-2013 Jason Perkins and the Premake project
 */

#include "premake.h"
#include <stdint.h>
#include <intrin.h>
#include <stdlib.h>
#include <inttypes.h>
#include <assert.h>
#include <DbgHelp.h>

#pragma comment(lib, "Dbghelp.lib")

enum CodeLocationType
{
	CLT_Lua,
	CLT_LuaAnonymous,
	CLT_C,
	CLT_Main,
};

typedef struct
{
	enum CodeLocationType type;
	const void* addr;
} CodeLocation;

typedef struct StackFrame_
{
	uint64_t start;
	uint64_t elapsed;
	struct StackFrame_* parent;
	struct StackFrame_* next;
	struct StackFrame_* child;
	CodeLocation code;
	uint64_t children_time;
	uint64_t overhead;
} StackFrame;

static StackFrame root_frame = {0};
static StackFrame* current_frame = &root_frame;

static void parseCodeLocation(lua_State* L, lua_Debug* ar, CodeLocation* cl)
{
	if (0 == strcmp(ar->what, "C"))
	{
		cl->type = CLT_C;
		cl->addr = lua_getcfunc(L);
	}
	else if (0 == strcmp(ar->what, "Lua"))
	{
		if (ar->name && ar->name[0] != '?')
		{
			cl->type = CLT_Lua;
			cl->addr = ar->name;
		}
		else
		{
			char buffer[LUA_IDSIZE + 11 + 1];
			const char* lastSlash;
			const char* source;

			source = ar->source;
			lastSlash = strrchr(ar->source, '/');
			if (lastSlash)
				source = lastSlash + 1;

			lua_getinfo(L, "l", ar);
			sprintf(buffer, "%s:%d", source, ar->currentline);

			cl->type = CLT_LuaAnonymous;
			cl->addr = strdup(buffer);
		}
	}
	else if (0 == strcmp(ar->what, "main"))
	{
		cl->type = CLT_Main;
		cl->addr = 0;
	}
	else
	{
		// unknown stack frame type
		assert(0);
		abort();
	}
}

static StackFrame* findChildInCurrent(lua_State* L, lua_Debug* ar)
{
	CodeLocation cl;
	StackFrame* child;

	parseCodeLocation(L, ar, &cl);

	// search for child
	for (child = current_frame->child; child != NULL; child = child->next)
	{
		if (cl.type == child->code.type && cl.addr == child->code.addr)
			return child;
	}

	// create new frame
	child = calloc(1, sizeof(StackFrame));
	child->parent = current_frame;
	child->next = current_frame->child;
	child->code = cl;
	current_frame->child = child;
	return child;
}

static void hook_enter(lua_State* L, lua_Debug* ar, int tail)
{
	StackFrame* frame;
	(void)(L, tail);
	uint64_t overhead;

	overhead = __rdtsc();
	lua_getinfo(L, "nS", ar);
	frame = findChildInCurrent(L, ar);
	current_frame = frame;
	frame->start = overhead;
	frame->overhead = __rdtsc() - overhead;
}

static void hook_leave(lua_State* L, lua_Debug* ar)
{
	uint64_t stop;
	(void)(L, ar);

	stop = __rdtsc();
	current_frame->elapsed += (stop - current_frame->start);
	current_frame = current_frame->parent;
	current_frame->overhead += (__rdtsc() - stop);
}

static void callhook(lua_State* L, lua_Debug* ar)
{
	switch (ar->event)
	{
	case LUA_HOOKCALL:
		hook_enter(L, ar, 0);
		break;
	case LUA_HOOKTAILRET:
		hook_leave(L, ar);
		break;
	case LUA_HOOKRET:
		hook_leave(L, ar);
		break;
	}
}

typedef struct
{
	char* name;
	uint64_t elapsed;
} FlatStack;

static int count_nodes(StackFrame* frame)
{
	int count;
	StackFrame* it;

	count = 1;
	for (it = frame->child; it != NULL; it = it->next) {
		count += count_nodes(it);
	}

	return count;
}

typedef struct ResolvedFrame_ {
	struct ResolvedFrame_* next;
	void* addr;
	char* name;
} ResolvedFrame;

static ResolvedFrame* g_resolved = NULL;

static char* resovle(void* addr)
{
	ResolvedFrame* resolvedFrame;
	SYMBOL_INFO* symbol;

	for (ResolvedFrame* it = g_resolved; it; it = it->next)
		if (it->addr == addr)
			return it->name;

	resolvedFrame = calloc(1, sizeof(resolvedFrame));
	resolvedFrame->addr = addr;

	symbol = calloc(1, sizeof(SYMBOL_INFO) + MAX_SYM_NAME);
	symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
	symbol->MaxNameLen = MAX_SYM_NAME;

	if (SymFromAddr(GetCurrentProcess(), addr, NULL, symbol))
	{
		resolvedFrame->name = strdup(symbol->Name);
	}
	else
	{
		sprintf((char*)symbol, "0x%p", addr);
		resolvedFrame->name = strdup((char*)symbol);
	}

	free(symbol);

	resolvedFrame->next = g_resolved;
	g_resolved = resolvedFrame;
	return resolvedFrame->name;
}

static int append_children(FlatStack* array, int index, StackFrame* frame, char* prefix)
{
	StackFrame* it;
	size_t namelen;
	char* myname;
	char* frame_name;
	char* frame_prefix;

	switch (frame->code.type)
	{
	case CLT_Lua:
		frame_name = (char*)frame->code.addr;
		frame_prefix = "LUA:";
		break;

	case CLT_LuaAnonymous:
		frame_name = (char*)frame->code.addr;
		frame_prefix = "LUA:";
		break;

	case CLT_C:
		frame_name = resovle(frame->code.addr);
		frame_prefix = "C:";
		break;

	case CLT_Main:
		frame_name = "Lua:main";
		frame_prefix = "LUA:";
		break;

	default:
		assert(0);
		abort();
	}

	/* construct name */
	namelen = strlen(prefix) + strlen(frame_name) + 2;
	myname = calloc(1, namelen);
	sprintf(myname, "%s;%s%s", prefix, frame_prefix, frame_name);

	/* add self */
	array[index].name = myname;
	array[index].elapsed = frame->elapsed;
	++index;

	/* add children */
	for (it = frame->child; it != NULL; it = it->next)
	{
		index = append_children(array, index, it, myname);
	}

	return index;
}

static void reduce_overhead(StackFrame* frame)
{
	frame->elapsed -= frame->overhead;
	for (StackFrame* it = frame->child; it != NULL; it = it->next)
	{
		reduce_overhead(it);
	}
}

static void calculate_child_time(StackFrame* frame)
{
	uint64_t child_time = 0;
	for (StackFrame* it = frame->child; it != NULL; it = it->next)
	{
		calculate_child_time(it);
		child_time = it->elapsed + it->children_time;
	}

	frame->children_time = child_time;
	frame->elapsed -= child_time;
}

static FlatStack* flatten_stacks(int* nstacks)
{
	FlatStack* stacks;
	int current_index;
	int count;

	count = 0;
	for (StackFrame* it = root_frame.child; it != NULL; it = it->next)
	{
		count += count_nodes(it);
	}
	*nstacks = count;
	stacks = calloc(count, sizeof(FlatStack));

	current_index = 0;
	for (StackFrame* it = root_frame.child; it != NULL; it = it->next)
	{
		current_index = append_children(stacks, current_index, it, "root");
	}

	return stacks;
}

static int qsort_compare_stacks(const FlatStack* a, const FlatStack* b)
{
	return strcmp(a->name, b->name);
}

static int fold_stacks(FlatStack* stacks, int nstacks)
{
	int read_index;
	int write_index;

	write_index = 0;
	for (read_index = 1; read_index != nstacks; ++read_index)
	{
		if (0 == strcmp(stacks[read_index].name, stacks[write_index].name))
		{
			stacks[write_index].elapsed += stacks[read_index].elapsed;
		}
		else
		{
			++write_index;
			stacks[write_index] = stacks[read_index];
		}
	}

	return write_index;
}

static void print_stacks(const char* fname, FlatStack* stacks, int nstacks)
{
	FILE* fp;

	fp = fopen(fname, "wb");
	if (fp)
	{
		int ii;
		for (ii = 0; ii != nstacks; ++ii)
			fprintf(fp, "%s %" PRIu64 "\n", stacks[ii].name, stacks[ii].elapsed);

		fclose(fp);
	}
}

int main(int argc, const char** argv)
{
	lua_State* L;
	int z;
	FlatStack* stacks;
	int nstacks;

	if (!SymInitialize(GetCurrentProcess(), NULL, TRUE))
	{
		printf("FAILED TO LOAD SYMBOLS\n");
	}

	L = luaL_newstate();
	luaL_openlibs(L);

	z = premake_init(L);
	if (z == OKAY) {
		lua_sethook(L, callhook, LUA_MASKCALL|LUA_MASKRET, 0);
		z = premake_execute(L, argc, argv, "src/_premake_main.lua");
	}

	reduce_overhead(&root_frame);
	calculate_child_time(&root_frame);

	stacks = flatten_stacks(&nstacks);
	qsort(stacks, nstacks, sizeof(FlatStack), qsort_compare_stacks);
	nstacks = fold_stacks(stacks, nstacks);
	print_stacks("F:/stacks.fg", stacks, nstacks);
	free(stacks);

	lua_close(L);
	return z;
}
