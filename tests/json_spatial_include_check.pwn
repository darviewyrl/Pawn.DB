#include <pawndb>
#pragma rational Float

native PDB:fixture_connection();
native PDBResult:fixture_result();
native probe(bool:ok);
native bool:equal(const actual[], const expected[]);
native bool:vector_close(Float:x, Float:y, Float:z);
forward OnRows();

main()
{
    new PDB:db = fixture_connection();
    new PDBResult:res = fixture_result();
    probe(pdb_activate_result(res));
    new durability;
    probe(pdb_json_fetch_int("doc", "$.inventory.slots[2].attributes.durability", durability));
    probe(durability == 12);
    probe(pdb_json_fetch_int("doc", "$.missing", durability));
    probe(durability == 0);
    new Float:ratio;
    probe(pdb_json_fetch_float("doc", "$.ratio", ratio));
    probe(ratio > 1.2499 && ratio < 1.2501);
    new bool:truth;
    probe(pdb_json_fetch_bool("doc", "$.truth", truth));
    probe(truth);
    new text[32];
    probe(pdb_json_get_str("doc", "$.id", text));
    probe(equal(text, "18446744073709551615"));
    new Float:x, Float:y, Float:z, Float:w;
    probe(pdb_json_get_vector("doc", "$.position", x, y, z));
    probe(x == 1.0 && y == 2.0 && z == 3.0);
    probe(pdb_json_get_vector4("doc", "$.transform", x, y, z, w));
    probe(w == 7.0);
    probe(pdb_get_vector("vec", x, y, z));
    probe(vector_close(x, y, z));
    probe(pdb_get_vector_by_index(1, x, y, z));
    probe(pdb_get_vector4("vec4", x, y, z, w));
    probe(w > 269.9999 && w < 270.0001);
    probe(pdb_get_vector4_by_index(2, x, y, z, w));
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
    x = 1.25;
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

public OnRows() { return pdb_num_rows(); }
