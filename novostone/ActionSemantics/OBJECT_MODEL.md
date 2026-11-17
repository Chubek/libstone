# Object model status

Objects, prototypes, classes, property descriptors, dispatch policies, inheritance,
traits and finalizers are not implemented. Cell identity is not an object model.
Closures and tagged references can express stateful functions but do not provide
property lookup or method dispatch.

Future object policies should be explicit semantic primitives over the common
heap, not hardcoded JavaScript or Java behavior. Cycles, getters/setters,
receiver binding and suspension roots need executable cross-backend tests.
