#include "Parameters.h"
#include "Debug.h"
#include "Util.h"
#include "CompareGOTerms.h"

int computeGOscore(int argc, const char **argv) {
    std::string usage("Computes GOscore for a clustering result\n");
    usage.append(
            "Written by Martin Steinegger (martin.steinegger@mpibpc.mpg.de) & Maria Hauser (mhauser@genzentrum.lmu.de).\n\n");
    usage.append("USAGE: <gofolder> <clustering_file> <prefix> <outputfolder>\n");
    Parameters par;
    par.parseParameters(argc, argv, usage, par.evaluationscores, 5);

    std::string gofolder = par.db1;
    Debug(Debug::INFO) << "GO folder is " << gofolder << "\n";

    std::string clustering_file = par.db2;
    Debug(Debug::INFO) << "Input clustering file is " << clustering_file << "\n";

    std::string prefix = par.db3;
    Debug(Debug::INFO) << "Prefix  is " << prefix << "\n";

    std::string outputfolder = par.db4;
    Debug(Debug::INFO) << "Outputfolder  is " << outputfolder << "\n";

    std::string sequencedb = par.db5;
    Debug(Debug::INFO) << "Sequence db  is " << sequencedb << "\n";

    usage.append(
            "-go <gofolder> <prot_go_folder> <clustering_file> <prefix> <outputfolder> <yes : all against all |no : representative against all(default) ><yes : randomized representative choice |no : representative against all(default) > \n");


    bool allagainstall = par.allVsAll;
    bool randomized = par.randomizedRepresentative;
    bool use_sequenceheader = par.use_sequenceheader;


    Debug(Debug::INFO) << "GO-Evaluation" << "\n";
    if (allagainstall) {
        Debug(Debug::INFO) << "all against all comparison";
    }
    if (randomized) {
        Debug(Debug::INFO) << "randomized representative comparison";
    }

    //"-go <gofolder> <prot_go_folder> <clustering_file> <prefix> <outputfolder>"
    //std::string gofolder="/home/lars/masterarbeit/data/GO/db/";
    //std::string uniprot_go_folder="/home/lars/masterarbeit/data/uniprot/release-2015_04/uniprot_go/";
    //"/home/lars/masterarbeit/data/uniprot/release-2015_04/evaluation"
    //      "/home/lars/masterarbeit/db/sprot/uniprot_sprot_s4_affinity"

    std::string *goCategories = new std::string[1];
    goCategories[0] = "_F";
    //goCategories[1] = "_C";
    //goCategories[2] = "_P";

    std::string *evidenceCategories = new std::string[3];
    evidenceCategories[0] = "_EXP";
    evidenceCategories[1] = "_NON-IEA";
    evidenceCategories[2] = "";

    int evidencenumber = 3;
    
    if( allagainstall) {
        evidencenumber=2;
     }

    for (int j = 0; j < evidencenumber; ++j) {
        for (int i = 0; i < 1; ++i) {
            CompareGOTerms go(gofolder + "go-fasta_db" + goCategories[i],
                                                    gofolder + "go-fasta_db" + goCategories[i] + ".index",
                                                    gofolder + "uniprot_sprot.dat_go_db" +
                                                    evidenceCategories[j] + goCategories[i],
                                                    gofolder + "uniprot_sprot.dat_go_db" +
                                                    evidenceCategories[j] + goCategories[i] + ".index",
                                                    outputfolder, sequencedb, use_sequenceheader);
            go.init();
            //go.all_against_all_comparison();
            //go.all_against_all_comparison_proteinset();
            go.run_evaluation_mmseqsclustering(clustering_file,
                                                clustering_file + ".index",
                                                prefix, evidenceCategories[j] + goCategories[i], allagainstall,
                                                randomized);
        }
    }

    Debug(Debug::INFO) << "GO-Evaluation finished" << "\n";
    return 0;
}
