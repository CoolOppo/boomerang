/*
 * Copyright (C) 2003, Mike Van Emmerik
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 *
 */

/***************************************************************************/ /**
  * \file       exphelp.h
  * OVERVIEW:   Element comparison functions for expressions and statements
  ******************************************************************************/

#ifndef __EXPHELP_H__
#define __EXPHELP_H__

#include <map>
#include <memory>
class Exp;
class Assign;
class Assignment;

/**
 * A class for comparing Exp*s (comparing the actual expressions)
 * Type sensitive
 */
struct lessExpStar : public std::binary_function<Exp *, Exp *, bool> {
    bool operator()(const Exp *x, const Exp *y) const;
};

struct lessExpShared : public std::binary_function<std::shared_ptr<Exp>, std::shared_ptr<Exp>, bool> {
    bool operator()(const std::shared_ptr<Exp> &x, const std::shared_ptr<Exp> &y) const;
};

/**
 * A class for comparing Exp*s (comparing the actual expressions)
 * Type insensitive
 */
struct lessTI : public std::binary_function<Exp *, Exp *, bool> {
    bool operator()(const Exp *x, const Exp *y) const;
};

//! Compare assignments by their left hand sides (only). Implemented in statement.cpp
struct lessAssignment : public std::binary_function<Assignment *, Assignment *, bool> {
    bool operator()(const Assignment *x, const Assignment *y) const;
};

// Repeat the above for Assigns; sometimes the #include ordering is such that the compiler doesn't know that an Assign
// is a subclass of Assignment
struct lessAssign : public std::binary_function<Assign *, Assign *, bool> {
    bool operator()(const Assign *x, const Assign *y) const;
};

#endif // __EXPHELP_H__
