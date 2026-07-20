void connectFoo() {
    auto foo = IFoo::getService("default");
    foo->linkToDeath(recipient, 0);
}

void publishFoo() {
    IFoo::registerAsService("default");
}

void Recipient::binderDied() {}
