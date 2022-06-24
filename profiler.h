#pragma once
#include "linkedlist.h"


#ifndef PICORO_ENABLE_PROFILER
#define PICORO_ENABLE_PROFILER  0
#endif

#if PICORO_ENABLE_PROFILER

// forward decl
struct FuncProfile;

extern volatile FuncProfile*    currentfunction;

/** Helper function called by PROFILE_THIS_FUNC. */
extern void linkup_func(FuncProfile* func);
/** Call this to print out the profile to stdout/console. */
extern void stop_and_dump_profile();


struct FuncProfile
{
    LinkedListEntry     llentry;
    unsigned int        entries;
    unsigned int        samples;
    const char*         name;

    FuncProfile(const char* name_)
        : entries(0), samples(0), name(name_)
    {
        linkup_func(this);
    }
};

struct ProfileStackframeHelper
{
    FuncProfile*    prev;

    ProfileStackframeHelper(FuncProfile* current)
        : prev((FuncProfile*) currentfunction)
    {
        currentfunction = current;
        current->entries++;
    }

    ~ProfileStackframeHelper()
    {
        currentfunction = prev;
    }
};

/**
 * Put PROFILE_THIS_FUNC at the top of your function.
 * Looks like:
 * \code
 * void funcA() {
 *      PROFILE_THIS_FUNC;
 *      do_stuff();
 * }
 * void funcB() {
 *      PROFILE_THIS_FUNC;
 *      funcA();
 *      do_other_stuff();
 * }
 * \endcode
 */
#define PROFILE_THIS_FUNC \
    static FuncProfile              __CONCAT(profile, __LINE__)(__PRETTY_FUNCTION__); \
    const ProfileStackframeHelper   __CONCAT(psh, __LINE__)(&__CONCAT(profile, __LINE__))


#else

#define PROFILE_THIS_FUNC   do {} while (false)

#endif
