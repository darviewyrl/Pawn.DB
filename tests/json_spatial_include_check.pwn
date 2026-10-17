#include <pawndb>
#pragma rational Float

native PDB:fixture_connection();
native probe(bool:ok);
native bool:equal(const actual[], const expected[]);
forward OnRows(PDBResult:res);

main()
{
    new PDB:db = fixture_connection();
    new PDBJson:j = pdb_json_create(true);
    probe(pdb_json_set_int(j, "n", 42));
    new output[256];
    probe(pdb_format(db, output, sizeof output, "%j", j) > 0);
    probe(equal(output, "'{\"n\":42}'"));
    probe(pdb_format(db, output, sizeof output, "' %j '", j) > 0);
    probe(equal(output, "'{\"n\":42}'"));
    probe(pdb_format(db, output, sizeof output, "%j", "{\"n\":1}") > 0);
    probe(equal(output, "'{\"n\":1}'"));
    probe(pdb_format(db, output, sizeof output, "'%j'", "broken") > 0);
    probe(equal(output, "NULL"));
    probe(pdb_format(db, output, sizeof output, "%j", PDB_JSON_INVALID) > 0);
    probe(equal(output, "NULL"));
    new Float:x = 1.25;
    probe(pdb_format(db, output, sizeof output, "' %v4 '", x, 2.0, 3.0, 90.0) > 0);
    probe(equal(output, "'1.25,2,3,90'"));
    probe(pdb_format(db, output, sizeof output, "%v %v3", x, 2.0, 3.0, x, 2.0, 3.0) > 0);
    probe(equal(output, "'1.25,2,3' '1.25,2,3'"));
    probe(pdb_format(db, output, sizeof output, "%d %f %b", 17, x, true) > 0);
    probe(equal(output, "17 1.25 1"));
    probe(pdb_format(db, output, sizeof output, "%v4", x, 2.0) == 0);
    probe(pdb_execute(db, "SELECT %j,%v3", j, x, 2.0, 3.0) != 0);
    probe(pdb_execute(db, "SELECT '%j'", j) != 0);
    probe(pdb_query(db, "SELECT %j", "OnRows", "", j) != 0);
    new PDBBatch:batch = pdb_batch_create(db, false);
    probe(pdb_batch_add(batch, "SELECT %j, ' %v4 '", j, x, 2.0, 3.0, 90.0));
    probe(pdb_batch_execute(batch));
    probe(pdb_json_free(j));
    probe(pdb_format(db, output, sizeof output, "'%j'", j) > 0);
    probe(equal(output, "NULL"));
}

public OnRows(PDBResult:res) { return _:res; }
