/*
 * Copyright (C) 2005, Mike Van Emmerik
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 *
 */

/***************************************************************************/ /**
  * \file       dataflow.cpp
  * \brief   Implementation of the DataFlow class
  ******************************************************************************/

#include "dataflow.h"

#include "cfg.h"
#include "proc.h"
#include "exp.h"
#include "boomerang.h"
#include "statement.h"
#include "visitor.h"
#include "log.h"
#include "basicblock.h"
#include "frontend.h"

#include <QtCore/QDebug>
#include <sstream>
#include <cstring>

extern char debug_buffer[]; // For prints functions

/*
 * Dominator frontier code largely as per Appel 2002 ("Modern Compiler Implementation in Java")
 */

DataFlow::~DataFlow()
{

}

void DataFlow::DFS(int p, size_t n) {
    if (dfnum[n] == 0) {
        dfnum[n] = N;
        vertex[N] = n;
        parent[n] = p;
        N++;
        // For each successor w of n
        BasicBlock *bb = BBs[n];
        const std::vector<BasicBlock *> &outEdges = bb->getOutEdges();
        for (BasicBlock * bb : outEdges) {
            DFS(n, indices[bb]);
        }
    }
}

// Essentially Algorithm 19.9 of Appel's "modern compiler implementation in Java" 2nd ed 2002
void DataFlow::dominators(Cfg *cfg) {
    BasicBlock *r = cfg->getEntryBB();
    size_t numBB = cfg->getNumBBs();
    BBs.resize(numBB, (BasicBlock *)-1);
    N = 0;
    BBs[0] = r;
    indices.clear(); // In case restart decompilation due to switch statements
    indices[r] = 0;
    // Initialise to "none"
    dfnum.resize(numBB, 0);
    semi.resize(numBB, -1);
    ancestor.resize(numBB, -1);
    idom.resize(numBB, -1);
    samedom.resize(numBB, -1);
    vertex.resize(numBB, -1);
    parent.resize(numBB, -1);
    best.resize(numBB, -1);
    bucket.resize(numBB);
    DF.resize(numBB);
    // Set up the BBs and indices vectors. Do this here because sometimes a BB can be unreachable (so relying on
    // in-edges doesn't work)
    std::list<BasicBlock *>::iterator ii;
    size_t idx = 1;
    for (ii = cfg->begin(); ii != cfg->end(); ii++) {
        BasicBlock *bb = *ii;
        if (bb != r) { // Entry BB r already done
            indices[bb] = idx;
            BBs[idx++] = bb;
        }
    }
    DFS(-1, 0);
    assert((N - 1) >= 0);
    for (int i = N - 1; i >= 1; i--) {
        int n = vertex[i];
        int p = parent[n];
        int s = p;
        /* These lines calculate the semi-dominator of n, based on the Semidominator Theorem */
        // for each predecessor v of n
        BasicBlock *bb = BBs[n];
        std::vector<BasicBlock *> &inEdges = bb->getInEdges();
        std::vector<BasicBlock *>::iterator it;
        for (it = inEdges.begin(); it != inEdges.end(); it++) {
            if (indices.find(*it) == indices.end()) {
                QTextStream q_cerr(stderr);

                q_cerr << "BB not in indices: ";
                (*it)->print(q_cerr);
                assert(false);
            }
            int v = indices[*it];
            int sdash = v;
            if (dfnum[v] > dfnum[n])
                sdash = semi[ancestorWithLowestSemi(v)];
            if (dfnum[sdash] < dfnum[s])
                s = sdash;
        }
        semi[n] = s;
        /* Calculation of n's dominator is deferred until the path from s to n has been linked into the forest */
        bucket[s].insert(n);
        Link(p, n);
        // for each v in bucket[p]
        std::set<int>::iterator jj;
        for (jj = bucket[p].begin(); jj != bucket[p].end(); jj++) {
            int v = *jj;
            /* Now that the path from p to v has been linked into the spanning forest, these lines calculate the
                                dominator of v, based on the first clause of the Dominator Theorem, or else defer the
               calculation until
                                y's dominator is known. */
            int y = ancestorWithLowestSemi(v);
            if (semi[y] == semi[v])
                idom[v] = p; // Success!
            else
                samedom[v] = y; // Defer
        }
        bucket[p].clear();
    }
    for (int i = 1; i < N - 1; i++) {
        /* Now all the deferred dominator calculations, based on the second clause of the Dominator Theorem, are
                        performed. */
        int n = vertex[i];
        if (samedom[n] != -1) {
            idom[n] = idom[samedom[n]]; // Deferred success!
        }
    }
    computeDF(0); // Finally, compute the dominance frontiers
}

// Basically algorithm 19.10b of Appel 2002 (uses path compression for O(log N) amortised time per operation
// (overall O(N log N))
int DataFlow::ancestorWithLowestSemi(int v) {
    int a = ancestor[v];
    if (ancestor[a] != -1) {
        int b = ancestorWithLowestSemi(a);
        ancestor[v] = ancestor[a];
        if (dfnum[semi[b]] < dfnum[semi[best[v]]])
            best[v] = b;
    }
    return best[v];
}

void DataFlow::Link(int p, int n) {
    ancestor[n] = p;
    best[n] = n;
}

// Return true if n dominates w
bool DataFlow::doesDominate(int n, int w) {
    while (idom[w] != -1) {
        if (idom[w] == n)
            return true;
        w = idom[w]; // Move up the dominator tree
    }
    return false;
}

void DataFlow::computeDF(int n) {
    std::set<int> S;
    /* THis loop computes DF_local[n] */
    // for each node y in succ(n)
    BasicBlock *bb = BBs[n];
    const std::vector<BasicBlock *> &outEdges = bb->getOutEdges();
    std::vector<BasicBlock *>::iterator it;
    for (BasicBlock *b : outEdges) {
        int y = indices[b];
        if (idom[y] != n)
            S.insert(y);
    }
    // for each child c of n in the dominator tree
    // Note: this is a linear search!
    int sz = idom.size(); // ? Was ancestor.size()
    for (int c = 0; c < sz; ++c) {
        if (idom[c] != n)
            continue;
        computeDF(c);
        /* This loop computes DF_up[c] */
        // for each element w of DF[c]
        std::set<int> &s = DF[c];
        std::set<int>::iterator ww;
        for (ww = s.begin(); ww != s.end(); ww++) {
            int w = *ww;
            // if n does not dominate w, or if n = w
            if (n == w || !doesDominate(n, w)) {
                S.insert(w);
            }
        }
    }
    DF[n] = S;
} // end computeDF

bool DataFlow::canRename(Exp *e, UserProc *proc) {
    if (e->isSubscript())
        e = ((RefExp *)e)->getSubExp1(); // Look inside refs
    if (e->isRegOf())
        return true; // Always rename registers
    if (e->isTemp())
        return true; // Always rename temps (always want to propagate away)
    if (e->isFlags())
        return true; // Always rename flags
    if (e->isMainFlag())
        return true; // Always rename individual flags like %CF
    if (e->isLocal())
        return true; // Rename hard locals in the post fromSSA pass
    if (!e->isMemOf())
        return false; // Can't rename %pc or other junk
    // I used to check here if there was a symbol for the memory expression, and if so allow it to be renamed. However,
    // even named locals and parameters could have their addresses escape the local function, so we need another test
    // anyway. So locals and parameters should not be renamed (and hence propagated) until escape analysis is done (and
    // hence renaleLocalsAndParams is set)
    // Besides,  before we have types and references, it is not easy to find a type for the location, so we can't tell
    // if e.g. m[esp{-}+12] is evnp or a separate local.
    // It certainly needs to have the local/parameter pattern
    if (!proc->isLocalOrParamPattern(e))
        return false;
    // e is a local or parameter; allow it to be propagated iff we've done escape analysis and the address has not
    return renameLocalsAndParams && !proc->isAddressEscapedVar(e); // escaped
}

// For debugging
void DataFlow::dumpA_phi() {
    std::map<Exp *, std::set<int>, lessExpStar>::iterator zz;
    LOG_STREAM() << "A_phi:\n";
    for (zz = A_phi.begin(); zz != A_phi.end(); ++zz) {
        LOG_STREAM() << zz->first << " -> ";
        std::set<int> &si = zz->second;
        std::set<int>::iterator qq;
        for (qq = si.begin(); qq != si.end(); ++qq)
            LOG_STREAM() << *qq << ", ";
        LOG_STREAM() << "\n";
    }
    LOG_STREAM() << "end A_phi\n";
}

bool DataFlow::placePhiFunctions(UserProc *proc) {
    // First free some memory no longer needed
    dfnum.resize(0);
    semi.resize(0);
    ancestor.resize(0);
    samedom.resize(0);
    vertex.resize(0);
    parent.resize(0);
    best.resize(0);
    bucket.resize(0);
    defsites.clear(); // Clear defsites map,
    defallsites.clear();
    for(std::set<Exp *, lessExpStar> &se : A_orig) {
        for(Exp * e : se) {
            if(A_phi.find(e)==A_phi.end())
                delete e;
        }
    }
    A_orig.clear();   // and A_orig,
    defStmts.clear(); // and the map from variable to defining Stmt

    bool change = false;

    // Set the sizes of needed vectors
    size_t numBB = indices.size();
    assert(numBB == proc->getCFG()->getNumBBs());
    A_orig.resize(numBB);

    // We need to create A_orig[n] for all n, the array of sets of locations defined at BB n
    // Recreate each call because propagation and other changes make old data invalid
    unsigned n;
    for (n = 0; n < numBB; n++) {
        BasicBlock::rtlit rit;
        StatementList::iterator sit;
        BasicBlock *bb = BBs[n];
        for (Instruction *s = bb->getFirstStmt(rit, sit); s; s = bb->getNextStmt(rit, sit)) {
            LocationSet ls;
            LocationSet::iterator it;
            s->getDefinitions(ls);
            if (s->isCall() && ((CallStatement *)s)->isChildless()) // If this is a childless call
                defallsites.insert(n);                              // then this block defines every variable
            for (it = ls.begin(); it != ls.end(); it++) {
                if (canRename(*it, proc)) {
                    A_orig[n].insert((*it)->clone());
                    defStmts[*it] = s;
                }
            }
        }
    }

    // For each node n
    for (n = 0; n < numBB; n++) {
        // For each variable a in A_orig[n]
        std::set<Exp *, lessExpStar> &s = A_orig[n];
        std::set<Exp *, lessExpStar>::iterator aa;
        for (aa = s.begin(); aa != s.end(); aa++) {
            Exp *a = *aa;
            defsites[a].insert(n);
        }
    }

    // For each variable a (in defsites, i.e. defined anywhere)
    std::map<Exp *, std::set<int>, lessExpStar>::iterator mm;
    for (mm = defsites.begin(); mm != defsites.end(); mm++) {
        Exp *a = (*mm).first; // *mm is pair<Exp*, set<int>>

        // Special processing for define-alls
        // for each n in defallsites
        std::set<int>::iterator da;
        for (da = defallsites.begin(); da != defallsites.end(); ++da)
            defsites[a].insert(*da);

        // W <- defsites[a];
        std::set<int> W = defsites[a]; // set copy
        // While W not empty
        while (W.size()) {
            // Remove some node n from W
            int n = *W.begin(); // Copy first element
            W.erase(W.begin()); // Remove first element
            // for each y in DF[n]
            std::set<int>::iterator yy;
            std::set<int> &DFn = DF[n];
            for (yy = DFn.begin(); yy != DFn.end(); yy++) {
                int y = *yy;
                // if y not element of A_phi[a]
                std::set<int> &s = A_phi[a];
                if (s.find(y) != s.end())
                    continue;
                // Insert trivial phi function for a at top of block y: a := phi()
                change = true;
                Instruction *as = new PhiAssign(a->clone());
                BasicBlock *Ybb = BBs[y];
                Ybb->prependStmt(as, proc);
                // A_phi[a] <- A_phi[a] U {y}
                s.insert(y);
                // if a !elementof A_orig[y]
                if (A_orig[y].find(a) == A_orig[y].end()) {
                    // W <- W U {y}
                    W.insert(y);
                }
            }
        }
    }
    return change;
} // end placePhiFunctions

static Exp *defineAll = new Terminal(opDefineAll); // An expression representing <all>

// There is an entry in stacks[defineAll] that represents the latest definition from a define-all source. It is needed
// for variables that don't have a definition as yet (i.e. stacks[x].empty() is true). As soon as a real definition to
// x appears, stacks[defineAll] does not apply for variable x. This is needed to get correct operation of the use
// collectors in calls.

// Care with the Stacks object (a map from expression to stack); using Stacks[q].empty() can needlessly insert an empty
// stack
#define STACKS_EMPTY(q) (Stacks.find(q) == Stacks.end() || Stacks[q].empty())

// Subscript dataflow variables
static int dataflow_progress = 0;
bool DataFlow::renameBlockVars(UserProc *proc, int n, bool clearStacks /* = false */) {
    if (++dataflow_progress > 200) {
        LOG_STREAM() << 'r';
        LOG_STREAM().flush();

        dataflow_progress = 0;
    }
    bool changed = false;

    // Need to clear the Stacks of old, renamed locations like m[esp-4] (these will be deleted, and will cause compare
    // failures in the Stacks, so it can't be correctly ordered and hence balanced etc, and will lead to segfaults)
    if (clearStacks)
        Stacks.clear();

    // For each statement S in block n
    BasicBlock::rtlit rit;
    StatementList::iterator sit;
    BasicBlock *bb = BBs[n];
    Instruction *S;
    for (S = bb->getFirstStmt(rit, sit); S; S = bb->getNextStmt(rit, sit)) {
        // if S is not a phi function (per Appel)
        /* if (!S->isPhi()) */ {
            // For each use of some variable x in S (not just assignments)
            LocationSet locs;
            if (S->isPhi()) {
                PhiAssign *pa = (PhiAssign *)S;
                Exp *phiLeft = pa->getLeft();
                if (phiLeft->isMemOf() || phiLeft->isRegOf())
                    phiLeft->getSubExp1()->addUsedLocs(locs);
                // A phi statement may use a location defined in a childless call, in which case its use collector
                // needs updating
                for (auto &pp : *pa) {
                    Instruction *def = pp.second.def();
                    if (def && def->isCall())
                        ((CallStatement *)def)->useBeforeDefine(phiLeft->clone());
                }
            } else { // Not a phi assignment
                S->addUsedLocs(locs);
            }
            LocationSet::iterator xx;
            for (xx = locs.begin(); xx != locs.end(); xx++) {
                Exp *x = *xx;
                // Don't rename memOfs that are not renamable according to the current policy
                if (!canRename(x, proc))
                    continue;
                Instruction *def = nullptr;
                if (x->isSubscript()) { // Already subscripted?
                    // No renaming required, but redo the usage analysis, in case this is a new return, and also because
                    // we may have just removed all call livenesses
                    // Update use information in calls, and in the proc (for parameters)
                    Exp *base = ((RefExp *)x)->getSubExp1();
                    def = ((RefExp *)x)->getDef();
                    if (def && def->isCall()) {
                        // Calls have UseCollectors for locations that are used before definition at the call
                        ((CallStatement *)def)->useBeforeDefine(base->clone());
                        continue;
                    }
                    // Update use collector in the proc (for parameters)
                    if (def == nullptr)
                        proc->useBeforeDefine(base->clone());
                    continue; // Don't re-rename the renamed variable
                }
                // Else x is not subscripted yet
                if (STACKS_EMPTY(x)) {
                    if (!Stacks[defineAll].empty())
                        def = Stacks[defineAll].back();
                    else {
                        // If the both stacks are empty, use a nullptr definition. This will be changed into a pointer
                        // to an implicit definition at the start of type analysis, but not until all the m[...]
                        // have stopped changing their expressions (complicates implicit assignments considerably).
                        def = nullptr;
                        // Update the collector at the start of the UserProc
                        proc->useBeforeDefine(x->clone());
                    }
                } else
                    def = Stacks[x].back();
                if (def && def->isCall())
                    // Calls have UseCollectors for locations that are used before definition at the call
                    ((CallStatement *)def)->useBeforeDefine(x->clone());
                // Replace the use of x with x{def} in S
                changed = true;
                if (S->isPhi()) {
                    Exp *phiLeft = ((PhiAssign *)S)->getLeft();
                    phiLeft->setSubExp1(phiLeft->getSubExp1()->expSubscriptVar(x, def /*, this*/));
                } else {
                    S->subscriptVar(x, def /*, this */);
                }
            }
        }

        // MVE: Check for Call and Return Statements; these have DefCollector objects that need to be updated
        // Do before the below, so CallStatements have not yet processed their defines
        if (S->isCall() || S->isReturn()) {
            DefCollector *col;
            if (S->isCall())
                col = ((CallStatement *)S)->getDefCollector();
            else
                col = ((ReturnStatement *)S)->getCollector();
            col->updateDefs(Stacks, proc);
        }

        // For each definition of some variable a in S
        LocationSet defs;
        S->getDefinitions(defs);
        LocationSet::iterator dd;
        for (dd = defs.begin(); dd != defs.end(); dd++) {
            Exp *a = *dd;
            // Don't consider a if it cannot be renamed
            bool suitable = canRename(a, proc);
            if (suitable) {
                // Push i onto Stacks[a]
                // Note: we clone a because otherwise it could be an expression that gets deleted through various
                // modifications. This is necessary because we do several passes of this algorithm to sort out the
                // memory expressions
                if(Stacks.find(a)!=Stacks.end()) // expression exists, no need for clone ?
                    Stacks[a].push_back(S);
                else
                    Stacks[a->clone()].push_back(S);
                // Replace definition of 'a' with definition of a_i in S (we don't do this)
            }
            // FIXME: MVE: do we need this awful hack?
            if (a->getOper() == opLocal) {
                const Exp *a1 = S->getProc()->expFromSymbol(((Const *)a->getSubExp1())->getStr());
                assert(a1);
                // Stacks already has a definition for a (as just the bare local)
                if (suitable) {
                    Stacks[a1->clone()].push_back(S);
                }
            }
        }
        // Special processing for define-alls (presently, only childless calls).
        // But note that only everythings at the current memory level are defined!
        if (S->isCall() && ((CallStatement *)S)->isChildless() && !Boomerang::get()->assumeABI) {
            // S is a childless call (and we're not assuming ABI compliance)
            Stacks[defineAll]; // Ensure that there is an entry for defineAll
            for (auto &elem : Stacks) {
                // if (dd->first->isMemDepth(memDepth))
                elem.second.push_back(S); // Add a definition for all vars
            }
        }
    }

    // For each successor Y of block n
    const std::vector<BasicBlock *> &outEdges = bb->getOutEdges();
    size_t numSucc = outEdges.size();
    for (unsigned succ = 0; succ < numSucc; succ++) {
        BasicBlock *Ybb = outEdges[succ];
        // For each phi-function in Y
        for (Instruction *St = Ybb->getFirstStmt(rit, sit); St; St = Ybb->getNextStmt(rit, sit)) {
            PhiAssign *pa = dynamic_cast<PhiAssign *>(St);
            // if S is not a phi function, then quit the loop (no more phi's)
            // Wrong: do not quit the loop: there's an optimisation that turns a PhiAssign into an ordinary Assign.
            // So continue, not break.
            if (!pa)
                continue;
            // Suppose the jth operand of the phi is 'a'
            // For now, just get the LHS
            Exp *a = pa->getLeft();

            // Only consider variables that can be renamed
            if (!canRename(a, proc))
                continue;
            Instruction *def = nullptr; // assume No reaching definition
            if (!STACKS_EMPTY(a))
                def = Stacks[a].back();

            // "Replace jth operand with a_i"
            pa->putAt(bb, def, a);
        }
    }

    // For each child X of n
    // Note: linear search!
    size_t numBB = proc->getCFG()->getNumBBs();
    for (size_t X = 0; X < numBB; X++) {
        if (idom[X] == n)
            renameBlockVars(proc, X);
    }

    // For each statement S in block n
    // NOTE: Because of the need to pop childless calls from the Stacks, it is important in my algorithm to process the
    // statments in the BB *backwards*. (It is not important in Appel's algorithm, since he always pushes a definition
    // for every variable defined on the Stacks).
    BasicBlock::rtlrit rrit;
    StatementList::reverse_iterator srit;
    for (S = bb->getLastStmt(rrit, srit); S; S = bb->getPrevStmt(rrit, srit)) {
        // For each definition of some variable a in S
        LocationSet defs;
        S->getDefinitions(defs);
        LocationSet::iterator dd;
        for (dd = defs.begin(); dd != defs.end(); dd++) {
            if (!canRename(*dd, proc))
                continue;
            // if ((*dd)->getMemDepth() == memDepth)
            auto ss = Stacks.find(*dd);
            if (ss == Stacks.end()) {
                LOG_STREAM() << "Tried to pop " << *dd << " from Stacks; does not exist\n";
                assert(0);
            }
            ss->second.pop_back();
        }
        // Pop all defs due to childless calls
        if (S->isCall() && ((CallStatement *)S)->isChildless()) {
            for (auto sss = Stacks.begin(); sss != Stacks.end(); ++sss) {
                if (!sss->second.empty() && sss->second.back() == S) {
                    sss->second.pop_back();
                }
            }
        }
    }
    return changed;
}

void DataFlow::dumpStacks() {
    LOG_STREAM() << "Stacks: " << Stacks.size() << " entries\n";
    for (auto zz = Stacks.begin(); zz != Stacks.end(); zz++) {
        LOG_STREAM() << "Var " << zz->first << " [ ";
        std::deque<Instruction *> tt = zz->second; // Copy the stack!
        while (!tt.empty()) {
            LOG_STREAM() << tt.back()->getNumber() << " ";
            tt.pop_back();
        }
        LOG_STREAM() << "]\n";
    }
}

void DataFlow::dumpDefsites() {
    std::map<Exp *, std::set<int>, lessExpStar>::iterator dd;
    for (dd = defsites.begin(); dd != defsites.end(); ++dd) {
        LOG_STREAM() << dd->first;
        std::set<int>::iterator ii;
        std::set<int> &si = dd->second;
        for (ii = si.begin(); ii != si.end(); ++ii)
            LOG_STREAM() << " " << *ii;
        LOG_STREAM() << "\n";
    }
}

void DataFlow::dumpA_orig() {
    int n = A_orig.size();
    for (int i = 0; i < n; ++i) {
        LOG_STREAM() << i;
        for (Exp *ee : A_orig[i])
            LOG_STREAM() << " " << ee;
        LOG_STREAM() << "\n";
    }
}

void DefCollector::updateDefs(std::map<Exp *, std::deque<Instruction *>, lessExpStar> &Stacks, UserProc *proc) {
    for (auto it = Stacks.begin(); it != Stacks.end(); it++) {
        if (it->second.empty())
            continue; // This variable's definition doesn't reach here
        // Create an assignment of the form loc := loc{def}
        RefExp *re = new RefExp(it->first->clone(), it->second.back());
        Assign *as = new Assign(it->first->clone(), re);
        as->setProc(proc); // Simplify sometimes needs this
        insert(as);
    }
    initialised = true;
}

// Find the definition for e that reaches this Collector. If none reaches here, return nullptr
Exp *DefCollector::findDefFor(Exp *e) {
    iterator it;
    for (it = defs.begin(); it != defs.end(); ++it) {
        Exp *lhs = (*it)->getLeft();
        if (*lhs == *e)
            return (*it)->getRight();
    }
    return nullptr; // Not explicitly defined here
}

/*
 * Print the collected locations to stream os
 */
void UseCollector::print(QTextStream &os, bool html) const {
    bool first = true;
    for (auto const &elem : locs) {
        if (first)
            first = false;
        else
            os << ",  ";
        (elem)->print(os, html);
    }
}

#define DEFCOL_COLS 120
//! Print the collected locations to stream os
void DefCollector::print(QTextStream &os, bool html) const {
    iterator it;
    unsigned col = 36;
    bool first = true;
    for (it = defs.begin(); it != defs.end(); ++it) {
        QString tgt;
        QTextStream ost(&tgt);
        (*it)->getLeft()->print(ost, html);
        ost << "=";
        (*it)->getRight()->print(ost, html);
        size_t len = tgt.length();
        if (first)
            first = false;
        else if (col + 4 + len >= DEFCOL_COLS) { // 4 for a comma and three spaces
            if (col != DEFCOL_COLS - 1)
                os << ","; // Comma at end of line
            os << "\n                ";
            col = 16;
        } else {
            os << ",   ";
            col += 4;
        }
        os << tgt;
        col += len;
    }
}

/*
 * Print to string or stderr (for debugging)
 */
char *UseCollector::prints() const {
    QString tgt;
    QTextStream ost(&tgt);
    print(ost);
    strncpy(debug_buffer, qPrintable(tgt), DEBUG_BUFSIZE - 1);
    debug_buffer[DEBUG_BUFSIZE - 1] = '\0';
    return debug_buffer;
}
//!Print to string or stdout (for debugging)
char *DefCollector::prints() const {
    QString tgt;
    QTextStream ost(&tgt);
    print(ost);
    strncpy(debug_buffer, qPrintable(tgt), DEBUG_BUFSIZE - 1);
    debug_buffer[DEBUG_BUFSIZE - 1] = '\0';
    return debug_buffer;
}

void UseCollector::dump() {
    QTextStream ost(stderr);
    print(ost);
}

void DefCollector::dump() {
    QTextStream ost(stderr);
    print(ost);
}

void UseCollector::makeCloneOf(UseCollector &other) {
    initialised = other.initialised;
    locs.clear();
    for (auto const &elem : other)
        locs.insert((elem)->clone());
}
/**
 * makeCloneOf(): clone the given Collector into this one
 */
void DefCollector::makeCloneOf(const DefCollector &other) {
    initialised = other.initialised;
    defs.clear();
    for (auto const &elem : other)
        defs.insert((Assign *)(elem)->clone());
}

void DefCollector::searchReplaceAll(const Exp &from, Exp *to, bool &change) {
    iterator it;
    for (it = defs.begin(); it != defs.end(); ++it)
        change |= (*it)->searchAndReplace(from, to);
}

// Called from CallStatement::fromSSAform. The UserProc is needed for the symbol map
void UseCollector::fromSSAform(UserProc *proc, Instruction *def) {
    LocationSet removes, inserts;
    iterator it;
    ExpSsaXformer esx(proc);
    for (it = locs.begin(); it != locs.end(); ++it) {
        RefExp *ref = new RefExp(*it, def); // Wrap it in a def
        Exp *ret = ref->accept(&esx);
        // If there is no change, ret will equal *it again (i.e. fromSSAform just removed the subscript)
        if (ret != *it) { // Pointer comparison
            // There was a change; we want to replace *it with ret
            removes.insert(*it);
            inserts.insert(ret);
        }
    }
    for (it = removes.begin(); it != removes.end(); ++it)
        locs.remove(*it);
    for (it = inserts.begin(); it != inserts.end(); ++it)
        locs.insert(*it);
}

bool UseCollector::operator==(UseCollector &other) {
    if (other.initialised != initialised)
        return false;
    iterator it1, it2;
    if (other.locs.size() != locs.size())
        return false;
    for (it1 = locs.begin(), it2 = other.locs.begin(); it1 != locs.end(); ++it1, ++it2)
        if (!(**it1 == **it2))
            return false;
    return true;
}

void DefCollector::insert(Assign *a) {
    Exp *l = a->getLeft();
    if (existsOnLeft(l))
        return;
    defs.insert(a);
}

void DataFlow::convertImplicits(Cfg *cfg) {
    // Convert statements in A_phi from m[...]{-} to m[...]{0}
    std::map<Exp *, std::set<int>, lessExpStar> A_phi_copy = A_phi; // Object copy
    ImplicitConverter ic(cfg);
    A_phi.clear();
    for (std::pair<Exp *, std::set<int>> it : A_phi_copy) {
        Exp *e = it.first->clone();
        e = e->accept(&ic);
        A_phi[e] = it.second; // Copy the set (doesn't have to be deep)
    }

    std::map<Exp *, std::set<int>, lessExpStar> defsites_copy = defsites; // Object copy
    defsites.clear();
    for (std::pair<Exp *, std::set<int>> dd : defsites_copy) {
        Exp *e = dd.first->clone();
        e = e->accept(&ic);
        defsites[e] = dd.second; // Copy the set (doesn't have to be deep)
    }

    std::vector<std::set<Exp *, lessExpStar>> A_orig_copy = A_orig;
    A_orig.clear();
    for (std::set<Exp *, lessExpStar> &se : A_orig_copy) {
        std::set<Exp *, lessExpStar> se_new;
        for (Exp *ee : se) {
            Exp *e = ee->clone();
            e = e->accept(&ic);
            se_new.insert(e);
        }
        A_orig.insert(A_orig.end(), se_new); // Copy the set (doesn't have to be a deep copy)
    }
}

// Helper function for UserProc::propagateStatements()
// Works on basic block n; call from UserProc with n=0 (entry BB)
// If an SSA location is in usedByDomPhi it means it is used in a phi that dominates its assignment
// However, it could turn out that the phi is dead, in which case we don't want to keep the associated entries in
// usedByDomPhi. So we maintain the map defdByPhi which maps locations defined at a phi to the phi statements. Every
// time we see a use of a location in defdByPhi, we remove that map entry. At the end of the procedure we therefore have
// only dead phi statements in the map, so we can delete the associated entries in defdByPhi and also remove the dead
// phi statements.
// We add to the set usedByDomPhi0 whenever we see a location referenced by a phi parameter. When we see a definition
// for such a location, we remove it from the usedByDomPhi0 set (to save memory) and add it to the usedByDomPhi set.
// For locations defined before they are used in a phi parameter, there will be no entry in usedByDomPhi, so we ignore
// it. Remember that each location is defined only once, so that's the time to decide if it is dominated by a phi use or
// not.
void DataFlow::findLiveAtDomPhi(int n, LocationSet &usedByDomPhi, LocationSet &usedByDomPhi0,
                                std::map<Exp *, PhiAssign *, lessExpStar> &defdByPhi) {
    // For each statement this BB
    BasicBlock::rtlit rit;
    StatementList::iterator sit;
    BasicBlock *bb = BBs[n];
    Instruction *S;
    for (S = bb->getFirstStmt(rit, sit); S; S = bb->getNextStmt(rit, sit)) {
        if (S->isPhi()) {
            // For each phi parameter, insert an entry into usedByDomPhi0
            PhiAssign *pa = (PhiAssign *)S;
            PhiAssign::iterator it;
            for (it = pa->begin(); it != pa->end(); ++it) {
                if (it->second.e) {
                    RefExp *re = new RefExp(it->second.e, it->second.def());
                    usedByDomPhi0.insert(re);
                }
            }
            // Insert an entry into the defdByPhi map
            RefExp *wrappedLhs = new RefExp(pa->getLeft(), pa);
            defdByPhi[wrappedLhs] = pa;
            // Fall through to the below, because phi uses are also legitimate uses
        }
        LocationSet ls;
        S->addUsedLocs(ls);
        // Consider uses of this statement
        for (Exp *it : ls) {
            // Remove this entry from the map, since it is not unused
            defdByPhi.erase(it);
        }
        // Now process any definitions
        ls.clear();
        S->getDefinitions(ls);
        for (Exp *it : ls) {
            RefExp wrappedDef(it, S);
            // If this definition is in the usedByDomPhi0 set, then it is in fact dominated by a phi use, so move it to
            // the final usedByDomPhi set
            if (usedByDomPhi0.find(&wrappedDef) != usedByDomPhi0.end()) {
                usedByDomPhi0.remove(&wrappedDef);
                usedByDomPhi.insert(new RefExp(it,S));
            }
        }
    }

    // Visit each child in the dominator graph
    // Note: this is a linear search!
    // Note also that usedByDomPhi0 may have some irrelevant entries, but this will do no harm, and attempting to erase
    // the irrelevant ones would probably cost more than leaving them alone
    int sz = idom.size();
    for (int c = 0; c < sz; ++c) {
        if (idom[c] != n)
            continue;
        // Recurse to the child
        findLiveAtDomPhi(c, usedByDomPhi, usedByDomPhi0, defdByPhi);
    }
}

void DataFlow::setDominanceNums(int n, int &currNum) {
#if USE_DOMINANCE_NUMS
    BasicBlock::rtlit rit;
    StatementList::iterator sit;
    BasicBlock *bb = BBs[n];
    Instruction *S;
    for (S = bb->getFirstStmt(rit, sit); S; S = bb->getNextStmt(rit, sit))
        S->setDomNumber(currNum++);
    int sz = idom.size();
    for (int c = 0; c < sz; ++c) {
        if (idom[c] != n)
            continue;
        // Recurse to the child
        setDominanceNums(c, currNum);
    }
#endif
}
