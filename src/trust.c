/* The trust-class law: the classes a witness may be declared under and the prior each plays at, generated from
 * manifest/trust_classes.toml. A label the manifest does not declare is no class: the lookup fails closed. */
#include "laplace/laplace.h"
#include <string.h>
#include "trust_class_law.h"

size_t lp_trust_class_count(void){ return sizeof lp_trust_class_law / sizeof *lp_trust_class_law; }
const lp_trust_class *lp_trust_class_at(size_t i){ return i < lp_trust_class_count() ? &lp_trust_class_law[i] : NULL; }
const lp_trust_class *lp_trust_class_named(const char *label){
    if (!label) return NULL;
    for (size_t i = 0; i < lp_trust_class_count(); i++) if (!strcmp(lp_trust_class_law[i].label, label)) return &lp_trust_class_law[i];
    return NULL;
}
