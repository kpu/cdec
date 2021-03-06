#include "incremental.h"

#include "hg.h"
#include "fdict.h"
#include "tdict.h"

#include "lm/enumerate_vocab.hh"
#include "lm/model.hh"
#include "search/config.hh"
#include "search/context.hh"
#include "search/edge.hh"
#include "search/edge_generator.hh"
#include "search/rule.hh"
#include "search/vertex.hh"
#include "search/vertex_generator.hh"
#include "util/exception.hh"

#include <boost/scoped_ptr.hpp>
#include <boost/scoped_array.hpp>

#include <iostream>
#include <vector>

namespace {

struct MapVocab : public lm::EnumerateVocab {
  public:
    MapVocab() {}

    // Do not call after Lookup.  
    void Add(lm::WordIndex index, const StringPiece &str) {
      const WordID cdec_id = TD::Convert(str.as_string());
      if (cdec_id >= out_.size()) out_.resize(cdec_id + 1);
      out_[cdec_id] = index;
    }

    // Assumes Add has been called and will never be called again.  
    lm::WordIndex FromCDec(WordID id) const {
      return out_[out_.size() > id ? id : 0];
    }

  private:
    std::vector<lm::WordIndex> out_;
};

class IncrementalBase {
  public:
    IncrementalBase(const std::vector<weight_t> &weights) : 
      cdec_weights_(weights),
      weights_(weights[FD::Convert("KLanguageModel")], weights[FD::Convert("KLanguageModel_OOV")], weights[FD::Convert("WordPenalty")]) {
      std::cerr << "Weights KLanguageModel " << weights_.LM() << " KLanguageModel_OOV " << weights_.OOV() << " WordPenalty " << weights_.WordPenalty() << std::endl;
    }

    virtual ~IncrementalBase() {}

    virtual void Search(unsigned int pop_limit, const Hypergraph &hg) const = 0;

    static IncrementalBase *Load(const char *model_file, const std::vector<weight_t> &weights);

  protected:
    lm::ngram::Config GetConfig() {
      lm::ngram::Config ret;
      ret.enumerate_vocab = &vocab_;
      return ret;
    }

    MapVocab vocab_;

    const std::vector<weight_t> &cdec_weights_;

    const search::Weights weights_;
};

template <class Model> class Incremental : public IncrementalBase {
  public:
    Incremental(const char *model_file, const std::vector<weight_t> &weights) : IncrementalBase(weights), m_(model_file, GetConfig()) {}

    void Search(unsigned int pop_limit, const Hypergraph &hg) const;

  private:
    void ConvertEdge(const search::Context<Model> &context, bool final, search::Vertex *vertices, const Hypergraph::Edge &in, search::EdgeGenerator &gen) const;

    const Model m_;
};

IncrementalBase *IncrementalBase::Load(const char *model_file, const std::vector<weight_t> &weights) {
  lm::ngram::ModelType model_type;
  if (!lm::ngram::RecognizeBinary(model_file, model_type)) model_type = lm::ngram::PROBING;
  switch (model_type) {
    case lm::ngram::PROBING:
      return new Incremental<lm::ngram::ProbingModel>(model_file, weights);
    case lm::ngram::REST_PROBING:
      return new Incremental<lm::ngram::RestProbingModel>(model_file, weights);
    default:
      UTIL_THROW(util::Exception, "Sorry this lm type isn't supported yet.");
  }
}

void PrintFinal(const Hypergraph &hg, const search::Final final) {
  const std::vector<WordID> &words = static_cast<const Hypergraph::Edge*>(final.GetNote().vp)->rule_->e();
  const search::Final *child(final.Children());
  for (std::vector<WordID>::const_iterator i = words.begin(); i != words.end(); ++i) {
    if (*i > 0) {
      std::cout << TD::Convert(*i) << ' ';
    } else {
      PrintFinal(hg, *child++);
    }
  }
}

template <class Model> void Incremental<Model>::Search(unsigned int pop_limit, const Hypergraph &hg) const {
  boost::scoped_array<search::Vertex> out_vertices(new search::Vertex[hg.nodes_.size()]);
  search::Config config(weights_, pop_limit);
  search::Context<Model> context(config, m_);

  for (unsigned int i = 0; i < hg.nodes_.size() - 1; ++i) {
    search::EdgeGenerator gen;
    const Hypergraph::EdgesVector &down_edges = hg.nodes_[i].in_edges_;
    for (unsigned int j = 0; j < down_edges.size(); ++j) {
      unsigned int edge_index = down_edges[j];
      ConvertEdge(context, i == hg.nodes_.size() - 2, out_vertices.get(), hg.edges_[edge_index], gen);
    }
    search::VertexGenerator vertex_gen(context, out_vertices[i]);
    gen.Search(context, vertex_gen);
  }
  const search::Final top = out_vertices[hg.nodes_.size() - 2].BestChild();
  if (!top.Valid()) {
    std::cout << "NO PATH FOUND" << std::endl;
  } else {
    PrintFinal(hg, top);
    std::cout << "||| " << top.GetScore() << std::endl;
  }
}

template <class Model> void Incremental<Model>::ConvertEdge(const search::Context<Model> &context, bool final, search::Vertex *vertices, const Hypergraph::Edge &in, search::EdgeGenerator &gen) const {
  const std::vector<WordID> &e = in.rule_->e();
  std::vector<lm::WordIndex> words;
  words.reserve(e.size());
  std::vector<search::PartialVertex> nts;
  unsigned int terminals = 0;
  float score = 0.0;
  for (std::vector<WordID>::const_iterator word = e.begin(); word != e.end(); ++word) {
    if (*word <= 0) {
      nts.push_back(vertices[in.tail_nodes_[-*word]].RootPartial());
      if (nts.back().Empty()) return;
      score += nts.back().Bound();
      words.push_back(lm::kMaxWordIndex);
    } else {
      ++terminals;
      words.push_back(vocab_.FromCDec(*word));
    }
  }

  if (final) {
    words.push_back(m_.GetVocabulary().EndSentence());
  }

  search::PartialEdge out(gen.AllocateEdge(nts.size()));

  memcpy(out.NT(), &nts[0], sizeof(search::PartialVertex) * nts.size());

  search::Note note;
  note.vp = &in;
  out.SetNote(note);

  score += in.rule_->GetFeatureValues().dot(cdec_weights_);
  score -= static_cast<float>(terminals) * context.GetWeights().WordPenalty() / M_LN10;
  score += search::ScoreRule(context, words, final, out.Between());
  out.SetScore(score);

  gen.AddEdge(out);
}

boost::scoped_ptr<IncrementalBase> AwfulGlobalIncremental;

} // namespace

void PassToIncremental(const char *model_file, const std::vector<weight_t> &weights, unsigned int pop_limit, const Hypergraph &hg) {
  if (!AwfulGlobalIncremental.get()) {
    std::cerr << "Pop limit " << pop_limit << std::endl;
    AwfulGlobalIncremental.reset(IncrementalBase::Load(model_file, weights));
  }
  AwfulGlobalIncremental->Search(pop_limit, hg);
}
