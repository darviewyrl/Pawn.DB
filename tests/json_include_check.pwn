#include <pawndb>
#pragma rational Float

main()
{
    new PDBJson:scoped = pdb_json_create();
    new PDBJson:retained = pdb_json_create(true);
    pdb_json_set_int(scoped, "int", 42);
    pdb_json_set_float(scoped, "float", 1.25);
    pdb_json_set_str(scoped, "str", "hello");
    pdb_json_set_bool(scoped, "bool", true);
    pdb_json_set_vector(scoped, "pos", 1.0, 2.0, 3.0);
    pdb_json_set_vector4(scoped, "transform", 1.0, 2.0, 3.0, 90.0);
    new output[256];
    pdb_json_to_string(scoped, output);
    pdb_json_retain(scoped);
    pdb_json_free(scoped);
    pdb_json_free(retained);
}
