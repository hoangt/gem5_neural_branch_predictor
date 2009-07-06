
#ifndef PTTOPTTOPOLOGY_H
#define PTTOPTTOPOLOGY_H

#include "mem/ruby/network/simple/Topology.hh"

class PtToPtTopology : public Topology
{
public:
  PtToPtTopology(const string & name);
  void init(const vector<string> & argv);

protected:
  void construct();
};

#endif