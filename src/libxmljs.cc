// Copyright 2009, Squish Tech, LLC.

#include <v8.h>

#include <libxml/xmlmemory.h>

#include "libxmljs.h"
#include "xml_document.h"
#include "xml_node.h"
#include "xml_sax_parser.h"

namespace libxmljs {

bool tlsInitialized = false;
nauv_key_t tlsKey;
bool isAsync = false; // Only set on V8 thread when no workers are running
int numWorkers = 0; // Only access from V8 thread

// Set up in V8 thread
WorkerParent::WorkerParent() : memAdjustments(0) {
    if (!tlsInitialized)
    {
        nauv_key_create(&tlsKey);
        tlsInitialized = true;
    }
    if (numWorkers++ == 0)
    {
        isAsync = true;
    }
}

// Tear down in V8 thread
WorkerParent::~WorkerParent() {
    NanAdjustExternalMemory(memAdjustments);
    if (--numWorkers == 0)
    {
        isAsync = false;
    }
}

// Set up in worker thread
WorkerSentinel::WorkerSentinel(WorkerParent& parent) : parent(parent) {
    nauv_key_set(&tlsKey, this);
}

// Tear down in worker thread
WorkerSentinel::~WorkerSentinel() {
    nauv_key_set(&tlsKey, NULL);
}

struct memHdr {
    size_t size;
    double data;
};

#define HDR_SIZE offsetof(memHdr, data)

inline void* hdr2client(memHdr* hdr) {
    return static_cast<void*>(reinterpret_cast<char*>(hdr) + HDR_SIZE);
}

inline memHdr* client2hdr(void* client) {
    return reinterpret_cast<memHdr*>(static_cast<char*>(client) - HDR_SIZE);
}

void adjustMem(ssize_t diff)
{
    if (isAsync)
    {
        WorkerSentinel* worker =
            static_cast<WorkerSentinel*>(nauv_key_get(&tlsKey));
        if (worker)
        {
            worker->parent.memAdjustments += diff;
            return;
        }
    }
    // if v8 is no longer running, don't try to adjust memory
    // this happens when the v8 vm is shutdown and the program is exiting
    // our cleanup routines for libxml will be called (freeing memory)
    // but v8 is already offline and does not need to be informed
    // trying to adjust after shutdown will result in a fatal error
#if (NODE_MODULE_VERSION > 0x000B)
    if (v8::Isolate::GetCurrent() == 0)
    {
        assert(diff <= 0);
        return;
    }
#endif
    if (v8::V8::IsDead())
    {
        assert(diff <= 0);
        return;
    }
    NanAdjustExternalMemory(diff);
}

void* memMalloc(size_t size)
{
    size_t totalSize = size + HDR_SIZE;
    memHdr* mem = static_cast<memHdr*>(malloc(totalSize));
    if (!mem) return NULL;
    mem->size = size;
    adjustMem(totalSize);
    return hdr2client(mem);
}

void memFree(void* p)
{
    memHdr* mem = client2hdr(p);
    ssize_t totalSize = mem->size + HDR_SIZE;
    adjustMem(-totalSize);
    free(mem);
}

void* memRealloc(void* ptr, size_t size)
{
    memHdr* mem1 = client2hdr(ptr);
    ssize_t oldSize = mem1->size;
    memHdr* mem2 = static_cast<memHdr*>(realloc(mem1, size + HDR_SIZE));
    if (!mem2) return NULL;
    mem2->size = size;
    adjustMem(ssize_t(size) - oldSize);
    return hdr2client(mem2);
}

char* memStrdup(const char* str)
{
    size_t size = strlen(str) + 1;
    char* res = static_cast<char*>(memMalloc(size));
    if (res) memcpy(res, str, size);
    return res;
}

// ensure destruction at exit time
// v8 doesn't cleanup its resources
LibXMLJS LibXMLJS::instance;

LibXMLJS::LibXMLJS()
{
    // Setup our own memory handling (see xmlmemory.h/c)
    xmlMemSetup(memFree, memMalloc, memRealloc, memStrdup);

    // initialize libxml
    LIBXML_TEST_VERSION;
}

LibXMLJS::~LibXMLJS()
{
    xmlCleanupParser();
}

v8::Local<v8::Object> listFeatures() {
    v8::Local<v8::Object> target = NanNew<v8::Object>();
#define FEAT(x) target->Set(NanNew<v8::String>(# x), \
                    NanNew<v8::Boolean>(xmlHasFeature(XML_WITH_ ## x)))
    // See enum xmlFeature in parser.h
    FEAT(THREAD);
    FEAT(TREE);
    FEAT(OUTPUT);
    FEAT(PUSH);
    FEAT(READER);
    FEAT(PATTERN);
    FEAT(WRITER);
    FEAT(SAX1);
    FEAT(FTP);
    FEAT(HTTP);
    FEAT(VALID);
    FEAT(HTML);
    FEAT(LEGACY);
    FEAT(C14N);
    FEAT(CATALOG);
    FEAT(XPATH);
    FEAT(XPTR);
    FEAT(XINCLUDE);
    FEAT(ICONV);
    FEAT(ISO8859X);
    FEAT(UNICODE);
    FEAT(REGEXP);
    FEAT(AUTOMATA);
    FEAT(EXPR);
    FEAT(SCHEMAS);
    FEAT(SCHEMATRON);
    FEAT(MODULES);
    FEAT(DEBUG);
    FEAT(DEBUG_MEM);
    FEAT(DEBUG_RUN);
    FEAT(ZLIB);
    FEAT(ICU);
    FEAT(LZMA);
    return target;
}

// used by node.js to initialize libraries
extern "C" void
init(v8::Handle<v8::Object> target)
{
      NanScope();

      XmlDocument::Initialize(target);
      XmlSaxParser::Initialize(target);

      target->Set(NanNew<v8::String>("libxml_version"),
                  NanNew<v8::String>(LIBXML_DOTTED_VERSION));

      target->Set(NanNew<v8::String>("libxml_parser_version"),
                  NanNew<v8::String>(xmlParserVersion));

      target->Set(NanNew<v8::String>("libxml_debug_enabled"),
                  NanNew<v8::Boolean>(debugging));

      target->Set(NanNew<v8::String>("features"), listFeatures());

      target->Set(NanNew<v8::String>("libxml"), target);
}

NODE_MODULE(xmljs, init)

}  // namespace libxmljs
